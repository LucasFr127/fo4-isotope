// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SharedUiFrame.hpp"
#include "SharedUiContent.hpp"
#include "SharedSdrFrame.hpp"
#include "RendererLifetime.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "DrawObservation.hpp"
#include "FrameTrace.hpp"
#include "ShaderRegistry.hpp"
#include "hooks/ContextHooks.hpp"
#include "resources/SharedUiComposition.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace fo4hdr {
namespace {

enum class SharedUiRejection : std::uint32_t
{
    none, nativeScope, scene, context, duplicateSeed, seedFailure, laterScene, incomplete, mutation
};

SharedUiComposition g_seed;
SharedUiContent g_content;
bool g_seedReady{};
std::atomic<float> g_startupSdrWhite{};
std::atomic<std::uint64_t> g_startupSdrRevision{}, g_startupSdrUncertainty{};
static_assert(std::atomic<float>::is_always_lock_free);
SharedUiResourceReadiness g_pauseResources;
std::atomic<std::uint64_t> g_seeds{}, g_presented{}, g_rejected{};
std::atomic<SharedUiRejection> g_rejection{};
SharedUiRejection g_frameRejection{};

struct PublicContentInterpretation
{
    SharedUiFrameIdentity identity;
    SharedUiPresentation presentation;
    std::uint64_t revision{}, uncertainty{}, resetEpoch{};
    DWORD thread{};
};
PublicContentInterpretation g_publicContent;
struct EmergencyPublicImage
{
    SharedUiPresentation presentation;
    std::uintptr_t resource{};
    std::uint64_t lifetime{}, revision{}, contextGeneration{};
    DWORD thread{};
};
EmergencyPublicImage g_emergencyImage;
std::atomic<std::uint64_t> g_publicLifetime{1};
std::atomic<std::uint64_t> g_publicRevision{}, g_publicUncertainty{};
SharedUiWriteEvidence g_sceneWrite;
std::atomic<std::uintptr_t> g_sceneIdentity{}, g_temporalIdentity{};
std::atomic<bool> g_foreignMutation{};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);

bool ProducerThread() noexcept
{
    const auto owner = g_observedPresentThread.load(std::memory_order_acquire);
    return owner == 0 || owner == GetCurrentThreadId();
}

struct StartupDeclaration
{
    SharedUiWriteEvidence write;
    float white{};
};

StartupDeclaration ReadStartupDeclaration() noexcept
{
    const auto revision = g_publicRevision.load(std::memory_order_acquire);
    const auto white = g_startupSdrWhite.load(std::memory_order_acquire);
    const auto uncertainty = g_startupSdrUncertainty.load(std::memory_order_acquire);
    if (g_startupSdrRevision.load(std::memory_order_acquire) != revision) return {};
    const bool assumed = uncertainty != g_publicUncertainty.load(std::memory_order_acquire);
    if (g_publicRevision.load(std::memory_order_acquire) != revision) return {};
    return {{revision, uncertainty, assumed}, white};
}

SharedUiFrameIdentity CurrentIdentity() noexcept
{
    const auto* record = FindDrawObservation(g_rendererDevice.activeContext);
    return {g_evidence.resourceGeneration.load(std::memory_order_acquire),
        record ? record->generation : 0, g_frame, ArmorUiScopeGeneration()};
}

void Reject(SharedUiRejection reason) noexcept
{
    g_content.rejected = true;
    g_frameRejection = reason;
    g_rejection.store(reason, std::memory_order_release);
}

bool LinearSceneReady() noexcept
{
    const auto identity = CurrentIdentity();
    const auto heartbeat = g_heartbeat.Evaluate(identity.generation, identity.frame);
    const std::array ready{g_referenceHealthy.load(), g_productionHealthy.load(),
        g_gameplayReadyApplied.load(std::memory_order_acquire),
        heartbeat.ready, g_scene.productionOperatorApplied,
        g_scene.currentScene != nullptr,
        g_scene.currentSceneDomain == LinearColorDomain::linearBt2020UiRelative,
        g_presentation.colorDomainGeneration == identity.generation};
    return std::ranges::all_of(ready, [](bool value) { return value; });
}

bool CompletedSceneReady() noexcept
{
    if (!LinearSceneReady()) return false;
    // An absent temporal pass does not invalidate a completed public scene; a failed one does.
    return g_scene.temporalSceneReady || (!g_scene.temporalPassObserved &&
        SameResourceIdentity(g_scene.currentScene.Get(), g_presentation.presentationProxySrv.Get()));
}

bool PublicLinearSceneReady() noexcept
{
    return g_presentation.presentationProxyDomain == LinearColorDomain::linearBt2020UiRelative &&
        SameResourceIdentity(g_scene.currentScene.Get(), g_presentation.presentationProxySrv.Get());
}

bool SceneReady(const ArmorUiScopeToken& token) noexcept
{
    const auto white = static_cast<float>(g_activeSettings.uiPaperWhiteNits);
    if (g_seedReady && !g_seed.InputsCompatible({g_scene.currentScene.Get(),
            g_presentation.presentationProxyRtv.Get(), {white, white}})) return false;
    return !g_foreignMutation.load(std::memory_order_acquire) && CompletedSceneReady() &&
        (token.owner != ArmorUiOwner::screenSpace || PublicLinearSceneReady());
}

bool ContextReady() noexcept
{
    auto* context = g_rendererDevice.activeContext;
    const auto* record = FindDrawObservation(context);
    return context && record && record->immediate &&
        g_liveSettingsThreadCompatible.load(std::memory_order_acquire);
}

bool UncompositedSceneReady() noexcept
{
    if (g_content.seeded || g_content.rejected) return false;
    if (g_foreignMutation.load(std::memory_order_acquire)) return false;
    const auto scope = CurrentArmorUiScope();
    const std::array ready{scope.depth == 0, CurrentIdentity().Valid(), ContextReady(),
        GetCurrentThreadId() == g_observedPresentThread.load(std::memory_order_acquire),
        CompletedSceneReady(), PublicLinearSceneReady()};
    return std::ranges::all_of(ready, [](bool value) { return value; });
}

bool SupportedOwner(const ArmorUiScopeToken& token) noexcept
{
    return token.owner == ArmorUiOwner::hud ||
        token.owner == ArmorUiOwner::powerArmor || token.owner == ArmorUiOwner::powerArmorRain ||
        ((token.owner == ArmorUiOwner::flatMenu || token.owner == ArmorUiOwner::workshop) &&
            token.targetArgument == 0);
}

bool NativeScopeReady(const ArmorUiScopeToken& token) noexcept
{
    const bool ownerReady = token.owner == ArmorUiOwner::screenSpace ||
        (SupportedOwner(token) && token.postAa && token.hasRoot);
    return ownerReady && token.depth == 1 && token.threadId == GetCurrentThreadId() &&
        token.requestGeneration == ArmorUiScopeGeneration();
}

bool EncodedSceneReady() noexcept
{
    constexpr auto domain = LinearColorDomain::encodedSrgbBt709UiRelative;
    return g_scene.currentSceneDomain == domain &&
        g_presentation.presentationProxyDomain == domain &&
        SameResourceIdentity(g_scene.currentScene.Get(), g_presentation.presentationProxySrv.Get());
}

SharedUiWriteEvidence CompleteSharedUiWrite(SharedUiWriteEvidence write) noexcept
{
    const auto completed = InvalidateSharedUiPresentation();
    if (completed.revision == write.revision + 1) write.revision = completed.revision;
    return write;
}

void PublishEncoded(SharedUiWriteEvidence write, float white) noexcept
{
    constexpr auto domain = LinearColorDomain::encodedSrgbBt709UiRelative;
    g_scene.currentScene = g_presentation.presentationProxySrv;
    g_scene.currentSceneDomain = domain;
    g_presentation.presentationProxyDomain = domain;
    g_presentation.presentationRedirectedThisFrame = true;
    g_evidence.colorDomain.store(static_cast<std::uint32_t>(domain), std::memory_order_release);
    g_seeds.fetch_add(1, std::memory_order_relaxed);
    g_rejection.store(SharedUiRejection::none, std::memory_order_release);
    g_frameRejection = SharedUiRejection::none;
    write.white = white;
    PublishSharedUiPublicImage({domain, white, true}, write);
    PublishSharedUiScene(write);
}

void BeginSharedUi(const ArmorUiScopeToken& token) noexcept
{
    if (!NativeScopeReady(token)) { Reject(SharedUiRejection::nativeScope); return; }
    if (!SceneReady(token)) { Reject(SharedUiRejection::scene); return; }
    if (!ContextReady()) { Reject(SharedUiRejection::context); return; }
    const auto white = static_cast<float>(g_activeSettings.uiPaperWhiteNits);
    auto write = CaptureSharedUiWrite();
    const SharedUiSeedInputs inputs{g_scene.currentScene.Get(),
        g_presentation.presentationProxyRtv.Get(), {write.white, white}};
    if (!g_content.Begin(CurrentIdentity(), token.serial, white))
    {
        Reject(SharedUiRejection::duplicateSeed);
        return;
    }
    auto* context = g_rendererDevice.activeContext;
    ScopedPresentationContext owned(context);
    if (!owned.Ready()) { Reject(SharedUiRejection::context); return; }
    if (!g_seedReady ||
        !g_seed.Seed(context, g_rendererDevice.activeContext1.Get(), inputs))
    {
        Reject(SharedUiRejection::seedFailure);
        return;
    }
    PublishEncoded(CompleteSharedUiWrite(write), white);
}

void EnterSharedUi(const ArmorUiScopeToken& token) noexcept
{
    // A scope that cannot seed may be empty or affect only unrelated resources.
    if (!NativeScopeReady(token)) return;
    if (g_content.seeded)
    {
        if (!ContextReady()) { Reject(SharedUiRejection::context); return; }
        if (token.owner == ArmorUiOwner::screenSpace && !EncodedSceneReady())
        {
            Reject(SharedUiRejection::scene);
            return;
        }
        if (!g_content.Open(CurrentIdentity(), token.serial)) Reject(SharedUiRejection::nativeScope);
        return;
    }
    if (SceneReady(token)) BeginSharedUi(token);
}

bool ProtectedSceneResource(ID3D11Resource* resource) noexcept
{
    if (!g_scene.temporalSceneReady && !g_scene.productionOperatorApplied) return false;
    if (!g_scene.currentScene) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> selected;
    g_scene.currentScene->GetResource(&selected);
    return SameResourceObjects(resource, selected.Get());
}

bool ProtectedSceneView(ID3D11View* view) noexcept
{
    if (!view) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    return ProtectedSceneResource(resource.Get());
}

bool BoundTargetsWriteProtectedScene() noexcept
{
    if (!g_rendererDevice.activeContext) return true;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    g_rendererDevice.activeContext->OMGetRenderTargets(
        static_cast<UINT>(targets.size()), targets.data(), nullptr);
    bool writesScene{};
    for (auto* target : targets)
    {
        writesScene |= ProtectedSceneView(target);
        if (target) target->Release();
    }
    return writesScene;
}

bool WritesProtectedScene(const DrawObservation& observation) noexcept
{
    if (!g_scene.temporalSceneReady && !g_scene.productionOperatorApplied) return false;
    if (ProtectedSceneView(observation.currentPrimaryRtv) ||
        ProtectedSceneView(observation.currentSecondaryRtv)) return true;
    return !observation.currentSinglePresentationTarget && BoundTargetsWriteProtectedScene();
}

void ObserveBeforeSeed(const DrawObservation& observation) noexcept
{
    if (!WritesProtectedScene(observation)) return;
    if (observation.currentPassKnown &&
        CurrentPassConsumesSelectedScene(g_rendererDevice.activeContext, observation)) return;
    Reject(SharedUiRejection::mutation);
}

void ObserveNativeWrite() noexcept
{
    const auto scope = CurrentArmorUiScope();
    // Once seeded, native UI shares the same representation as foreign UI.
    if (g_armor.armorEncodedActive && !SharedUiScopeActive(scope))
        Reject(SharedUiRejection::nativeScope);
    else g_content.ObserveNativeWrite(g_content.scope);
}

bool ForeignArmorScope() noexcept
{
    return g_armor.armorEncodedActive && g_armor.armorEncodedSharedSrgb &&
        CurrentArmorUiScope().serial != g_armor.armorEncodedScope;
}

bool BoundInputsReadProtectedScene() noexcept
{
    // shortcut: unknown nested shaders use bound-SRV overlap until shader-use metadata is available.
    std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> inputs{};
    g_rendererDevice.activeContext->PSGetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
    bool readsScene{};
    for (auto* input : inputs)
    {
        readsScene |= ProtectedSceneView(input);
        if (input) input->Release();
    }
    return readsScene;
}

bool KnownInputsReadProtectedScene(RenoDxShaderPass pass) noexcept
{
    const auto* descriptor = FindShaderDescriptor(pass);
    if (!descriptor) return false;
    UINT slot{};
    for (auto mask = descriptor->texture2dInputMask; mask != 0; mask >>= 1, ++slot)
    {
        if ((mask & 1) == 0) continue;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input;
        g_rendererDevice.activeContext->PSGetShaderResources(slot, 1, &input);
        if (ProtectedSceneView(input.Get())) return true;
    }
    return false;
}

void ObserveAfterSeed(const DrawObservation& observation) noexcept
{
    const bool writesScene = WritesProtectedScene(observation);
    if (observation.currentPassKnown)
    {
        const bool affectsScene = writesScene ||
            KnownInputsReadProtectedScene(observation.currentPass);
        if (affectsScene) Reject(SharedUiRejection::laterScene);
    }
    else if (ForeignArmorScope() && BoundInputsReadProtectedScene())
        Reject(SharedUiRejection::nativeScope);
    if (writesScene) ObserveNativeWrite();
}

float TracePaperWhite(FrameTraceSharedUiDecision decision) noexcept
{
    if (decision == FrameTraceSharedUiDecision::startupSdr) return SharedUiStartupPaperWhite();
    if (decision == FrameTraceSharedUiDecision::hdr && !g_content.seeded)
        return static_cast<float>(g_activeSettings.uiPaperWhiteNits);
    return g_content.white;
}

void TraceCompletion(FrameTraceSharedUiDecision decision, bool outputWritten) noexcept
{
    if (!FrameTraceRequested()) return;
    const bool uncomposited = decision == FrameTraceSharedUiDecision::hdr && !g_content.seeded;
    const auto identity = uncomposited ? CurrentIdentity() : g_content.identity;
    TraceSharedUiPresentation({.contentRevision = uncomposited ? 0 : g_content.revision,
        .generation = identity.generation, .contextGeneration = identity.context,
        .frame = identity.frame, .frozenWhite = TracePaperWhite(decision),
        .seeded = g_content.seeded, .closed = g_content.closed, .nativeWritten = g_content.nativeWritten,
        .rejected = g_content.rejected, .outputWritten = outputWritten,
        .assumed = decision == FrameTraceSharedUiDecision::startupSdr && SharedUiStartupAssumed(),
        .rejection = static_cast<std::uint32_t>(g_frameRejection),
        .decision = decision, .context = g_rendererDevice.activeContext,
        .selectedScene = g_scene.currentScene.Get(), .publicBuffer = g_presentation.presentationProxy.Get(),
        .outputBuffer = g_presentation.presentationBackBuffer.Get()});
}

SharedUiWriteEvidence AssumeSharedUiPresentation() noexcept
{
    return {g_publicRevision.load(std::memory_order_acquire),
        g_publicUncertainty.fetch_add(1, std::memory_order_acq_rel) + 1, true};
}

void StoreStartupDeclaration(float white, SharedUiWriteEvidence write) noexcept
{
    if (g_publicRevision.load(std::memory_order_acquire) != write.revision) return;
    g_startupSdrRevision.store(write.revision, std::memory_order_release);
    g_startupSdrUncertainty.store(write.uncertainty, std::memory_order_release);
    g_startupSdrWhite.store(white, std::memory_order_release);
    if (g_publicRevision.load(std::memory_order_acquire) != write.revision)
        g_startupSdrWhite.store(0.0F, std::memory_order_release);
}

void PreserveStartupClear(bool verified, SharedUiWriteEvidence write) noexcept
{
    if (!verified || g_publicRevision.load(std::memory_order_acquire) != write.revision) return;
    if (g_publicUncertainty.load(std::memory_order_acquire) != write.uncertainty) return;
    auto previous = write.uncertainty - 1;
    (void)g_startupSdrUncertainty.compare_exchange_strong(previous, write.uncertainty,
        std::memory_order_acq_rel);
}

bool PublicProducerReady() noexcept
{
    const auto domain = g_scene.currentSceneDomain;
    const std::array ready{ProducerThread(), ApplicationPresentationFacadeActive(),
        g_gameplayReadyApplied.load(std::memory_order_acquire),
        g_sceneWrite.revision == g_publicRevision.load(std::memory_order_acquire),
        SameResourceIdentity(g_scene.currentScene.Get(), g_presentation.presentationProxySrv.Get()),
        domain == LinearColorDomain::linearBt2020UiRelative ||
            domain == LinearColorDomain::encodedSrgbBt709UiRelative};
    return std::ranges::all_of(ready, [](bool value) { return value; });
}

} // namespace

SharedUiWriteEvidence InvalidateSharedUiPresentation() noexcept
{
    g_startupSdrWhite.store(0.0F, std::memory_order_release);
    auto write = CaptureSharedUiWrite();
    write.revision = g_publicRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
    return write;
}

SharedUiWriteEvidence CaptureSharedUiWrite(bool freshScene, float white) noexcept
{
    const auto uncertainty = g_publicUncertainty.load(std::memory_order_acquire);
    if (!ProducerThread()) return {g_publicRevision.load(std::memory_order_acquire), uncertainty, true};
    const auto* record = FindDrawObservation(g_rendererDevice.activeContext);
    if (white <= 0.0F)
        white = !freshScene && g_sceneWrite.white > 0.0F ? g_sceneWrite.white :
            static_cast<float>(g_activeSettings.uiPaperWhiteNits);
    return {g_publicRevision.load(std::memory_order_acquire), uncertainty,
        !freshScene && (g_sceneWrite.assumed || g_sceneWrite.uncertainty != uncertainty),
        g_publicLifetime.load(std::memory_order_acquire),
        reinterpret_cast<std::uintptr_t>(g_presentation.presentationProxy.Get()),
        record ? record->generation : 0, white};
}

void RetireSharedUiPublicImage() noexcept
{
    g_publicLifetime.fetch_add(1, std::memory_order_acq_rel);
    (void)InvalidateSharedUiPresentation();
    if (ProducerThread()) g_emergencyImage = {};
}

bool PublicImageWriteCurrent(const SharedUiWriteEvidence& write) noexcept
{
    return ProducerThread() && write.publicLifetime != 0 &&
        write.publicLifetime == g_publicLifetime.load(std::memory_order_acquire) &&
        write.publicResource != 0 && write.publicResource ==
            reinterpret_cast<std::uintptr_t>(g_presentation.presentationProxy.Get()) &&
        write.revision >= g_emergencyImage.revision;
}

void PublishSharedUiPublicImage(SharedUiPresentation presentation, SharedUiWriteEvidence write) noexcept
{
    if (!PublicImageWriteCurrent(write)) return;
    const std::array valid{presentation.domain != LinearColorDomain::unknown,
        std::isfinite(presentation.white), presentation.white > 0.0F};
    if (!std::ranges::all_of(valid, [](bool value) { return value; })) return;
    presentation.assumed = true;
    g_emergencyImage = {presentation, write.publicResource, write.publicLifetime,
        write.revision, write.contextGeneration, GetCurrentThreadId()};
}

SharedUiWriteEvidence PublishSharedUiConversion(SharedUiPresentation presentation,
    SharedUiWriteEvidence write) noexcept
{
    write = CompleteSharedUiWrite(write);
    PublishSharedUiPublicImage(presentation, write);
    return write;
}

std::optional<SharedUiPresentation> SharedUiEmergencyPresentation() noexcept
{
    if (!ProducerThread()) return std::nullopt;
    const auto* record = FindDrawObservation(g_rendererDevice.activeContext);
    const std::array ready{g_referenceHealthy.load(),
        g_rendererDevice.activeContext != nullptr, !g_armor.armorEncodedActive,
        record && record->immediate,
        g_emergencyImage.lifetime == g_publicLifetime.load(std::memory_order_acquire),
        g_emergencyImage.resource != 0 && g_emergencyImage.resource ==
            reinterpret_cast<std::uintptr_t>(g_presentation.presentationProxy.Get()),
        record && record->generation == g_emergencyImage.contextGeneration,
        g_emergencyImage.thread == GetCurrentThreadId(),
        g_emergencyImage.presentation.white > 0.0F};
    if (!std::ranges::all_of(ready, [](bool value) { return value; })) return std::nullopt;
    return g_emergencyImage.presentation;
}

void PublishSharedUiScene(SharedUiWriteEvidence write) noexcept
{
    if (!ProducerThread()) return;
    write.assumed |= write.uncertainty != g_publicUncertainty.load(std::memory_order_acquire);
    g_sceneWrite = write;
    Microsoft::WRL::ComPtr<ID3D11Resource> selected;
    const bool sceneReady = g_scene.productionOperatorApplied || g_scene.temporalSceneReady;
    if (sceneReady && g_scene.currentScene)
        g_scene.currentScene->GetResource(&selected);
    g_sceneIdentity.store(CanonicalResourceIdentity(selected.Get()), std::memory_order_release);
    g_temporalIdentity.store(g_scene.temporalSceneReady ?
        CanonicalResourceIdentity(selected.Get()) : 0, std::memory_order_release);
    if (SameResourceIdentity(g_scene.currentScene.Get(), g_presentation.presentationProxySrv.Get()))
        PublishSharedUiPublicImage({g_scene.currentSceneDomain, write.white, true}, write);
    PublishSharedUiPresentation();
}

void PublishSharedUiPresentation() noexcept
{
    if (!ProducerThread()) return;
    if (!PublicProducerReady()) return;
    const auto domain = g_scene.currentSceneDomain;
    // Successful output publication may have advanced the resource generation.
    const auto white = domain == LinearColorDomain::encodedSrgbBt709UiRelative ?
        g_content.white : g_sceneWrite.white;
    const bool assumed = g_sceneWrite.assumed ||
        g_sceneWrite.uncertainty != g_publicUncertainty.load(std::memory_order_acquire);
    g_publicContent = {CurrentIdentity(), {g_scene.currentSceneDomain, white, assumed},
        g_sceneWrite.revision, g_sceneWrite.uncertainty,
        ReadGraphicsLifecycle().resetEpoch, GetCurrentThreadId()};
}

std::optional<SharedUiPresentation> SharedUiPresentationForOutput() noexcept
{
    const auto current = CurrentIdentity();
    const auto& saved = g_publicContent.identity;
    const std::array ready{ApplicationPresentationFacadeActive(), g_referenceHealthy.load(),
        g_gameplayReadyApplied.load(std::memory_order_acquire), ContextReady(), !g_armor.armorEncodedActive,
        saved.Valid(), current.generation == saved.generation, current.context == saved.context,
        current.request == saved.request, current.frame >= saved.frame,
        g_publicContent.thread == GetCurrentThreadId(),
        g_publicContent.resetEpoch == ReadGraphicsLifecycle().resetEpoch,
        g_publicContent.revision == g_publicRevision.load(std::memory_order_acquire)};
    if (!std::ranges::all_of(ready, [](bool value) { return value; })) return std::nullopt;
    auto presentation = g_publicContent.presentation;
    presentation.assumed |= g_publicContent.uncertainty != g_publicUncertainty.load(std::memory_order_acquire);
    if (g_publicContent.revision != g_publicRevision.load(std::memory_order_acquire)) return std::nullopt;
    return presentation;
}

bool EnsureSharedUiResources(ID3D11Device* device) noexcept
{
    if (!g_configuration.productionRequested) return true;
    if (!g_seedReady)
        g_seedReady = g_seed.Create(device, g_presentation.width, g_presentation.height);
    return g_seedReady;
}

void ResetSharedUiContent() noexcept
{
    const auto startup = ReadStartupDeclaration();
    const auto cleared = InvalidateSharedUiPresentation();
    if (startup.white > 0.0F && cleared.revision == startup.write.revision + 1)
        StoreStartupDeclaration(startup.white, {cleared.revision, startup.write.uncertainty});
    g_publicContent = {};
    g_sceneWrite = cleared;
    g_sceneIdentity.store(0, std::memory_order_release);
    g_temporalIdentity.store(0, std::memory_order_release);
    g_foreignMutation.store(false, std::memory_order_release);
    ResetSharedSdrFrame();
    g_content = {};
    g_frameRejection = SharedUiRejection::none;
}

bool InitializeSharedUiSdr(ID3D11DeviceContext* context) noexcept
{
    const auto* record = FindDrawObservation(context);
    if (!record || !record->immediate) return false;
    if (!record->computeQueries.CanDispatch()) return false;
    if (!g_presentation.presentationProxyRtv) return false;
    const auto white = static_cast<float>(g_activeSettings.uiPaperWhiteNits);
    if (!std::isfinite(white) || white <= 0.0F) return false;
    // Suppress observation of this owned clear when the context is already published.
    ScopedPresentationContext owned(context);
    auto cleared = InvalidateSharedUiPresentation();
    cleared.contextGeneration = record->generation;
    ClearWholePublicBuffer(context);
    PublishSharedUiPublicImage({LinearColorDomain::vanillaSdrCodes, white, true}, cleared);
    StoreStartupDeclaration(white, cleared);
    return SharedUiStartupPaperWhite() > 0.0F;
}

void ResetSharedUiResources(bool retainContentProvenance) noexcept
{
    ResetSharedUiContent();
    g_seed.Reset();
    g_seedReady = false;
    if (!retainContentProvenance) g_startupSdrWhite = 0.0F;
}

void SharedUiBoundary(const ArmorUiScopeToken& token, bool entering) noexcept
{
    // Other native threads cannot inspect the renderer's immediate-context transaction.
    if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire)) return;
    if (!ApplicationPresentationFacadeActive()) return;
    if (entering) EnterSharedUi(token);
    else if (g_content.seeded && token.serial == g_content.scope)
    {
        if (!NativeScopeReady(token)) Reject(SharedUiRejection::nativeScope);
        g_content.Close(CurrentIdentity(), token.serial);
    }
}

namespace {

bool ReplacementWritesPublic(const DrawObservation& observation) noexcept
{
    return observation.currentReplacementActive &&
        (IsPresentationProxyTarget(observation.currentPrimaryRtv) ||
            IsPresentationProxyTarget(observation.currentSecondaryRtv));
}

void ObserveForeignMutation(ID3D11Resource* destination, bool publicDestination) noexcept
{
    const auto identity = CanonicalResourceIdentity(destination);
    if (!identity) return;
    const bool overlaps = identity == g_sceneIdentity.load(std::memory_order_acquire) ||
        identity == g_temporalIdentity.load(std::memory_order_acquire);
    if (overlaps)
    {
        if (!publicDestination) (void)AssumeSharedUiPresentation();
        g_foreignMutation.store(true, std::memory_order_release);
    }
}

bool ObservePublicMutation(ID3D11Resource* destination, bool nativeClear) noexcept
{
    if (!SameResourceObjects(destination, g_presentation.presentationProxy.Get())) return false;
    const bool verifiedStartup = nativeClear && ProducerThread() && SharedUiStartupSdr();
    PreserveStartupClear(verifiedStartup, AssumeSharedUiPresentation());
    return true;
}

bool ObserveMutationThread(ID3D11Resource* destination, bool publicDestination) noexcept
{
    if (GetCurrentThreadId() == g_observedPresentThread.load(std::memory_order_acquire)) return true;
    if (publicDestination) g_foreignMutation.store(true, std::memory_order_release);
    ObserveForeignMutation(destination, publicDestination);
    return false;
}

void RejectProtectedMutation(ID3D11Resource* destination, bool publicDestination) noexcept
{
    const bool seededPublic = publicDestination && g_content.seeded;
    if (!seededPublic && !ProtectedSceneResource(destination)) return;
    if (!publicDestination) (void)AssumeSharedUiPresentation();
    Reject(SharedUiRejection::mutation);
}

// An upscaler's copy-back keeps the assumed linear scene; seeding it fixes later UI blending.
bool SeedablePublicCopy(bool publicDestination, ID3D11Resource* source) noexcept
{
    return publicDestination && source && !g_content.seeded && PublicLinearSceneReady();
}

} // namespace

bool ObserveSharedUiDraw(const DrawObservation& observation) noexcept
{
    if (!ApplicationPresentationFacadeActive()) return false;
    // UI qualification does not change the public image's established interpretation.
    if (ReplacementWritesPublic(observation))
    {
        InvalidateSharedUiPresentation();
    }
    if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire))
    {
        g_foreignMutation.store(true, std::memory_order_release);
        return false;
    }
    if (g_foreignMutation.load(std::memory_order_acquire)) Reject(SharedUiRejection::mutation);
    if (g_content.seeded) ObserveAfterSeed(observation);
    else ObserveBeforeSeed(observation);
    return g_content.rejected;
}

bool ObserveSharedUiCopy(ID3D11Resource* destination, ID3D11Resource* source) noexcept
{
    if (!ApplicationPresentationFacadeActive()) return false;
    ObserveSharedSdrCopy(destination);
    const bool publicDestination = ObservePublicMutation(destination, false);
    if (!ObserveMutationThread(destination, publicDestination)) return false;
    if (!SeedablePublicCopy(publicDestination, source))
        RejectProtectedMutation(destination, publicDestination);
    if (ForeignArmorScope() && ProtectedSceneResource(source)) Reject(SharedUiRejection::mutation);
    return g_content.rejected;
}

void ObserveSharedUiClear(ID3D11RenderTargetView* target) noexcept
{
    if (!ApplicationPresentationFacadeActive()) return;
    ObserveSharedSdrClear(target);
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    if (target) target->GetResource(&resource);
    const bool publicDestination = ObservePublicMutation(resource.Get(), true);
    if (!ObserveMutationThread(resource.Get(), publicDestination)) return;
    RejectProtectedMutation(resource.Get(), publicDestination);
}

bool SharedUiStartupSdr() noexcept
{
    const auto startup = ReadStartupDeclaration();
    return startup.white > 0.0F && !startup.write.assumed;
}

bool SharedUiStartupAssumed() noexcept
{
    const auto startup = ReadStartupDeclaration();
    return startup.white > 0.0F && startup.write.assumed;
}

std::optional<SharedUiPresentation> SharedUiStartupPresentation() noexcept
{
    const auto startup = ReadStartupDeclaration();
    if (startup.white <= 0.0F) return std::nullopt;
    return SharedUiPresentation{LinearColorDomain::vanillaSdrCodes, startup.white, startup.write.assumed};
}

float SharedUiStartupPaperWhite() noexcept
{
    return ReadStartupDeclaration().white;
}

void RefreshSharedUiStartupWhite(SharedUiWriteEvidence cleared) noexcept
{
    const auto white = SharedSdrPaperWhite();
    if (white > 0.0F)
    {
        PublishSharedUiPublicImage({LinearColorDomain::vanillaSdrCodes, white, true}, cleared);
        StoreStartupDeclaration(white, cleared);
    }
}

void SetSharedUiPauseResourcesReady(bool ready) noexcept
{
    g_pauseResources.SetReady(ready);
}

void DisableSharedUiPauseResources() noexcept
{
    g_pauseResources.Disable();
}

bool SharedUiPauseResourcesReady() noexcept
{
    return g_pauseResources.Ready();
}

bool SharedUiFrameReady() noexcept
{
    const std::array ready{ApplicationPresentationFacadeActive(), !g_armor.armorEncodedActive,
        !g_foreignMutation.load(std::memory_order_acquire),
        g_sceneWrite.revision == g_publicRevision.load(std::memory_order_acquire),
        !g_sceneWrite.assumed, g_sceneWrite.uncertainty == g_publicUncertainty.load(std::memory_order_acquire),
        g_content.Ready(CurrentIdentity()) || UncompositedSceneReady()};
    return std::ranges::all_of(ready, [](bool value) { return value; });
}

bool SharedUiFrameEncoded() noexcept
{
    return g_presentation.presentationProxyDomain == LinearColorDomain::encodedSrgbBt709UiRelative;
}

float SharedUiPaperWhite() noexcept
{
    return g_content.white;
}

bool SharedUiScopeActive(const ArmorUiScopeToken& token) noexcept
{
    return NativeScopeReady(token) && g_content.ScopeActive(CurrentIdentity(), token.serial);
}

void RejectSharedUiFrame() noexcept
{
    Reject(SharedUiRejection::nativeScope);
}

bool ScaleSharedSdrVideo(ID3D11DeviceContext* context, float videoWhite, float uiWhite) noexcept
{
    if (videoWhite == uiWhite) return true;
    const auto startup = ReadStartupDeclaration();
    if (startup.white <= 0.0F) return false;
    ScopedPresentationContext owned(context);
    if (!owned.Ready()) return false;
    auto write = CaptureSharedUiWrite(false, videoWhite);
    write.revision = startup.write.revision;
    write.uncertainty = startup.write.uncertainty;
    write.assumed = startup.write.assumed;
    if (g_seedReady && g_seed.Seed(context, g_rendererDevice.activeContext1.Get(),
        {g_presentation.presentationProxySrv.Get(), g_presentation.presentationProxyRtv.Get(),
            {videoWhite, uiWhite}, SharedUiSeedMode::scaleVanillaSdr}))
    {
        const auto completed = CompleteSharedUiWrite(write);
        PublishSharedUiPublicImage({LinearColorDomain::vanillaSdrCodes, uiWhite, true}, completed);
        StoreStartupDeclaration(uiWhite, completed);
        return true;
    }
    return false;
}

void TracePublicCompletion(const SharedUiPresentation* output) noexcept
{
    if (!FrameTraceRequested()) return;
    const auto& presentation = output ? *output : g_publicContent.presentation;
    const auto identity = CurrentIdentity();
    TraceSharedUiPresentation({.contentRevision = g_emergencyImage.revision,
        .generation = identity.generation, .contextGeneration = identity.context,
        .frame = identity.frame, .frozenWhite = presentation.white,
        .seeded = g_content.seeded, .closed = g_content.closed, .nativeWritten = g_content.nativeWritten,
        .rejected = g_content.rejected || g_foreignMutation.load(std::memory_order_acquire),
        .outputWritten = true, .assumed = presentation.assumed,
        .rejection = static_cast<std::uint32_t>(g_frameRejection),
        .decision = FrameTraceSharedUiDecision::hdrPublicContent,
        .context = g_rendererDevice.activeContext, .selectedScene = g_presentation.presentationProxySrv.Get(),
        .publicBuffer = g_presentation.presentationProxy.Get(), .outputBuffer = g_presentation.presentationBackBuffer.Get()});
}

void TraceSharedUiOutput(SharedUiOutput output, const SharedUiPresentation* presentation) noexcept
{
    if (!ApplicationPresentationFacadeActive()) return;
    if (output.publicContent)
    {
        TracePublicCompletion(presentation);
        return;
    }
    if (TraceSharedSdrOutput(output.rendered, output.written, output.startup)) return;
    if (!output.rendered && g_frameRejection == SharedUiRejection::none)
        g_frameRejection = SharedUiRejection::incomplete;
    const auto decision = output.rendered ? FrameTraceSharedUiDecision::hdr :
        (output.startup ? FrameTraceSharedUiDecision::startupSdr : FrameTraceSharedUiDecision::black);
    TraceCompletion(decision, output.written);
}

void CompleteSharedUiFrame(bool rendered) noexcept
{
    if (!ApplicationPresentationFacadeActive()) return;
    CompleteSharedSdrFrame(rendered);
    if (rendered) g_presented.fetch_add(1, std::memory_order_relaxed);
    else
    {
        g_rejected.fetch_add(1, std::memory_order_relaxed);
        g_rejection.store(g_frameRejection == SharedUiRejection::none ?
            SharedUiRejection::incomplete : g_frameRejection, std::memory_order_release);
    }
    g_content.Advance();
    g_sceneIdentity.store(0, std::memory_order_release);
    g_temporalIdentity.store(0, std::memory_order_release);
    g_foreignMutation.store(false, std::memory_order_release);
    g_frameRejection = SharedUiRejection::none;
}

void ReportSharedUi(GraphicsDiagnosticSink sink)
{
    if (!g_configuration.developerMode) return;
    if (!g_configuration.productionRequested || !sink) return;
    ReportSharedSdr(sink);
    constexpr std::array names{"none", "native-scope", "scene", "context", "duplicate-seed",
        "seed-failure", "later-scene", "incomplete", "mutation"};
    sink(std::format("shared-ui seeds={} presented={} rejected={} last_rejection={} pause_blur_ready={}",
        g_seeds.load(), g_presented.load(), g_rejected.load(),
        names[static_cast<std::size_t>(g_rejection.load())], SharedUiPauseResourcesReady()));
}

} // namespace fo4hdr
