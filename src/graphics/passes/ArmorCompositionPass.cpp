// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArmorCompositionPass.hpp"
#include "ArmorEncodedComposition.hpp"
#include "../resources/ArmorCompositionResources.hpp"
#include "../resources/PresentationResources.hpp"
#include "../resources/SceneResources.hpp"
#include "../RendererLifetime.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LiveSettings.hpp"
#include "../DrawObservation.hpp"
#include <cmath>
#include <cstring>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

void InvalidateArmorEncodedComposition() noexcept
{
    g_armor.armorEncodedActive = false;
    g_armor.armorEncodedFailed = true;
    g_armor.armorEncodedStatus = "Disabled: encoded scope escaped or decode failed";
    g_scene.currentScene.Reset();
    g_scene.currentSceneDomain = LinearColorDomain::unknown;
    g_presentation.presentationProxyDomain = LinearColorDomain::unknown;
    // No GPU work on an unqualified thread/context. The normal Present
    // handoff must clear the real backbuffer instead of consuming this proxy.
    g_presentation.presentationRedirectedThisFrame = true;
    g_evidence.colorDomain.store(static_cast<std::uint32_t>(LinearColorDomain::unknown),
        std::memory_order_release);
    g_referenceHealthy = false;
    RecordFailure(GraphicsObservationFailure::colorDomainMismatch);
}


void ArmorEncodedBoundary(const ArmorUiScopeToken& token, bool entering) noexcept;

struct ArmorBoundaryObservation
{
    const ArmorUiScopeToken& token;
    DWORD thread;
    ID3D11DeviceContext* context;
    DrawObservation* record;
    std::uint64_t generation;
    bool validContext;

    bool TokenReady() const noexcept
    {
        return token.postAa && token.hasRoot && token.threadId == thread &&
            g_liveSettingsThreadCompatible.load(std::memory_order_acquire) &&
            token.requestGeneration == ArmorUiScopeGeneration();
    }

    bool ContextCanDispatch() const noexcept
    {
        return validContext && OriginalRenderTargetSetter(context) && record->computeQueries.CanDispatch();
    }

    bool RendererReady() const noexcept
    {
        return HdrPathRequested() && g_referenceHealthy && !g_armor.armorEncodedActive &&
            !g_armor.armorEncodedFailed && ContextCanDispatch();
    }

    bool SceneReady() const noexcept
    {
        return g_scene.productionOperatorApplied && g_scene.currentScene &&
            g_scene.currentScene.Get() == g_presentation.presentationProxySrv.Get() &&
            g_scene.currentSceneDomain == LinearColorDomain::linearBt2020UiRelative &&
            g_presentation.presentationProxyDomain == LinearColorDomain::linearBt2020UiRelative &&
            generation != 0 && g_presentation.colorDomainGeneration == generation;
    }

    static bool PositiveFinite(double value) noexcept
    {
        return std::isfinite(value) && value > 0.0;
    }

    bool PeakSettingsReady() const noexcept
    {
        return g_armor.armorEncodedConstants[0] &&
            PositiveFinite(g_activeSettings.displayPeakNits) &&
            PositiveFinite(g_activeSettings.uiPaperWhiteNits);
    }

    bool PinDecodePeak(float peakUiRelative) const noexcept
    {
        if (g_armor.armorEncodedPeakUiRelative == peakUiRelative) return true;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(g_armor.armorEncodedConstants[0].Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        g_armor.armorEncodedPeakUiRelative = 0.0F;
        if (!mapped.pData)
        {
            context->Unmap(g_armor.armorEncodedConstants[0].Get(), 0);
            return false;
        }
        const ArmorEncodedConstants constants{0, peakUiRelative, {}};
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(g_armor.armorEncodedConstants[0].Get(), 0);
        g_armor.armorEncodedPeakUiRelative = peakUiRelative;
        return true;
    }

    bool Dispatch(UINT constantIndex) const noexcept
    {
        return DispatchArmorEncodedComposition(context, g_armor.armorEncodedContext1.Get(),
            {g_armor.armorEncodedShader.Get(), g_presentation.presentationProxyUav.Get(),
            g_armor.armorEncodedConstants[constantIndex].Get(), g_presentation.width, g_presentation.height},
            OriginalRenderTargetSetter(context));
    }

    void PublishScope() const noexcept
    {
        g_armor.armorEncodedActive = true;
        g_armor.armorEncodedScope = token.serial;
        g_armor.armorEncodedThread = thread;
        g_armor.armorEncodedContextGeneration = record->generation;
        g_armor.armorEncodedGeneration = generation;
        g_armor.armorEncodedFrame = g_frame;
        g_armor.armorEncodedStatus = "Encoded native armor scope";
    }

    bool EntryReady() const noexcept
    {
        return TokenReady() && RendererReady() && SceneReady();
    }

    bool PrepareDecodePeak() const noexcept
    {
        g_armor.armorEncodedStatus = "Invalid armor peak or unavailable decode constants";
        if (!PeakSettingsReady()) return false;
        const auto peakUiRelative = static_cast<float>(
            g_activeSettings.displayPeakNits / g_activeSettings.uiPaperWhiteNits);
        if (!PositiveFinite(peakUiRelative)) return false;
        // Exit uses this pinned completed-output limit even if settings change.
        return PinDecodePeak(peakUiRelative);
    }

    void Enter() const noexcept
    {
        if (token.owner != ArmorUiOwner::powerArmor || token.depth != 1) return;
        if (g_armor.armorEncodedFailed) return;
        g_armor.armorEncodedStatus = "Waiting for qualified post-AA proxy";
        if (!EntryReady()) return;
        if (!PrepareDecodePeak()) return;
        g_armor.armorEncodedStatus = "Unsupported typed UAV or unsafe bindings";
        if (!Dispatch(1)) return;
        PublishScope();
    }

    bool ExitContextSafe() const noexcept
    {
        return ContextCanDispatch() &&
            record->generation == g_armor.armorEncodedContextGeneration;
    }

    bool ExitScopeMatches() const noexcept
    {
        return token.serial == g_armor.armorEncodedScope &&
            (token.threadId == 0 || token.threadId == g_armor.armorEncodedThread);
    }

    void Exit() const noexcept
    {
        const auto action = DecideArmorUiCompositionExit({
            .active = g_armor.armorEncodedActive, .scopeMatches = ExitScopeMatches(),
            .ownerThreadMatches = thread == g_armor.armorEncodedThread,
            .contextSafe = ExitContextSafe(),
            .generationMatches = generation == g_armor.armorEncodedGeneration,
            .frameMatches = g_frame == g_armor.armorEncodedFrame});
        if (action == ArmorUiCompositionExit::ignore) return;
        const bool decoded = action == ArmorUiCompositionExit::decode && Dispatch(0);
        g_armor.armorEncodedActive = false;
        if (decoded)
            g_armor.armorEncodedStatus = "Complete: two in-place conversions";
        else
            InvalidateArmorEncodedComposition();
    }

    void RejectNestedOwner() const noexcept
    {
        // A nested owner ends the outer transaction; its eventual exit is a no-op.
        auto outer = token;
        outer.serial = g_armor.armorEncodedScope;
        ArmorEncodedBoundary(outer, false);
        g_armor.armorEncodedFailed = true;
        g_armor.armorEncodedStatus = "Disabled: nested native owner inside armor scope";
    }
};

bool ArmorContextValid(DrawObservation* record, ID3D11DeviceContext* context) noexcept
{
    return record && record->immediate && context && FindDrawObservation(context) == record &&
        context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

void ArmorEncodedBoundary(const ArmorUiScopeToken& token, bool entering) noexcept
{
    const auto thread = GetCurrentThreadId();
    // Foreign engine threads cannot inspect non-atomic renderer transactions.
    if (thread != g_observedPresentThread.load(std::memory_order_acquire)) return;
    auto* context = g_rendererDevice.activeContext;
    auto* record = context ? FindDrawObservation(context) : nullptr;
    const auto generation = g_evidence.resourceGeneration.load(std::memory_order_acquire);
    const auto validContext = ArmorContextValid(record, context);
    const ArmorBoundaryObservation observation{token, thread, context, record, generation, validContext};
    if (entering && g_armor.armorEncodedActive)
    {
        observation.RejectNestedOwner();
        return;
    }
    if (entering) observation.Enter();
    else observation.Exit();
}

// Renderer-owned lifecycle transitions invalidate stranded native transactions.
void CloseArmorEncodedCompositionForLifecycle() noexcept
{
    const auto thread = GetCurrentThreadId();
    const auto action = DecideArmorUiCompositionExit({
        .active = g_armor.armorEncodedActive, .scopeMatches = true,
        .ownerThreadMatches = thread == g_armor.armorEncodedThread &&
            thread == g_observedPresentThread.load(std::memory_order_acquire),
        .contextSafe = true, .generationMatches = true, .frameMatches = true});
    if (action == ArmorUiCompositionExit::invalidate)
        InvalidateArmorEncodedComposition();
    else if (action == ArmorUiCompositionExit::decode)
    {
        ArmorUiScopeToken token{};
        token.serial = g_armor.armorEncodedScope;
        token.threadId = g_armor.armorEncodedThread;
        ArmorEncodedBoundary(token, false);
    }
}

} // namespace fo4hdr
