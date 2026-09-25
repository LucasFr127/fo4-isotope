// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "ContextHooks.hpp"

#include "DeviceHooks.hpp"
#include "HookRecords.hpp"
#include "../DisplayDiscovery.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LiveSettings.hpp"
#include "../RendererLifetime.hpp"
#include "../ShaderRegistry.hpp"
#include "../passes/ArmorCompositionPass.hpp"
#include "../passes/ObservedDraw.hpp"
#include "../resources/ArmorCompositionResources.hpp"
#include "../resources/PresentationResources.hpp"
#include "../resources/ReplacementShaders.hpp"
#include "../resources/SceneResources.hpp"

#include <wrl/client.h>
#include <array>

namespace fo4hdr {

using Microsoft::WRL::ComPtr;

ULONG STDMETHODCALLTYPE HookContextRelease(
    ID3D11DeviceContext* context) noexcept
{
    return InvokeNoexceptBoundary<ULONG>(1, [=]() {
        auto* record = FindContext(context);
        if (record == nullptr || record->nextRelease == nullptr) return 1UL;
        const auto generation = record->identity.Generation();
        const auto remaining = InvokeCapturedNext(record->nextRelease, context);
        if (remaining == 0)
        {
            (void)record->identity.Retire(context, generation);
            if (g_pipboyWatchedContext.load(std::memory_order_acquire) == generation)
                g_pipboyRetiredContext.store(generation, std::memory_order_release);
        }
        return remaining;
    }, []() noexcept { RecordAbiException(); });
}

void ObserveQueryBegin(
    ContextQuerySafety& queries, ID3D11Asynchronous* asynchronous)
{
    ComPtr<ID3D11Query> query;
    if (asynchronous == nullptr ||
        FAILED(asynchronous->QueryInterface(IID_PPV_ARGS(&query))))
    {
        queries.unknown = true;
        return;
    }
    D3D11_QUERY_DESC description{};
    query->GetDesc(&description);
    if (IsComputeSensitiveQuery(description.Query))
        queries.Begin(asynchronous);
    else if (description.Query != D3D11_QUERY_TIMESTAMP_DISJOINT)
        queries.unknown = true;
}

void STDMETHODCALLTYPE HookBegin(
    ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextBegin == nullptr) return;
    try
    {
        ObserveQueryBegin(record->computeQueries, asynchronous);
    }
    catch (...)
    {
        record->computeQueries.unknown = true;
        RecordAbiException();
    }
    try { InvokeCapturedNext(record->nextBegin, context, asynchronous); }
    catch (...)
    {
        record->computeQueries.unknown = true;
        RecordAbiException();
    }
}

void STDMETHODCALLTYPE HookEnd(
    ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextEnd == nullptr) return;
    // Retire only after next: a downstream hook can issue draws during End.
    try
    {
        InvokeCapturedNext(record->nextEnd, context, asynchronous);
        record->computeQueries.End(asynchronous);
    }
    catch (...)
    {
        record->computeQueries.unknown = true;
        RecordAbiException();
    }
}

void STDMETHODCALLTYPE HookExecuteCommandList(
    ID3D11DeviceContext* context, ID3D11CommandList* commands,
    BOOL restoreState) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextExecuteCommandList == nullptr) return;
    // FinishCommandList closes deferred query scopes. Executing a finished
    // list therefore does not introduce an unbounded unknown immediate scope.
    // Block re-entrant compute only while next executes; preserve our immediate
    // Begin/End set, including scopes that make a conflicting list invalid.
    const bool entered = record->computeQueries.EnterCommandList();
    bool completed{};
    try
    {
        InvokeCapturedNext(record->nextExecuteCommandList,
            context, commands, restoreState);
        completed = true;
    }
    catch (...)
    {
        record->computeQueries.unknown = true;
        RecordAbiException();
    }
    if (entered) record->computeQueries.LeaveCommandList();
    if (!restoreState || !completed)
    {
        // FALSE resets the API pipeline to defaults; do not retain cached
        // pre-list shader/target observations as though they were still bound.
        record->currentPrimaryRtv = nullptr;
        record->currentSecondaryRtv = nullptr;
        record->currentSinglePresentationTarget = false;
        record->currentPass = {};
        record->currentPassKnown = false;
        record->currentReplacementActive = false;
        record->expectedBoundShader = nullptr;
        record->currentRequirement = PipelinePassRequirement::untracked;
    }
}

bool ArmorScopeOnPresentThread(ID3D11DeviceContext* context) noexcept
{
    return g_armor.armorEncodedActive && context == g_rendererDevice.activeContext &&
        GetCurrentThreadId() == g_observedPresentThread.load(std::memory_order_acquire);
}

void CloseArmorForSceneShader(ID3D11DeviceContext* context)
{
    if (ArmorScopeOnPresentThread(context))
    {
        ArmorUiScopeToken outer{};
        outer.serial = g_armor.armorEncodedScope;
        ArmorEncodedBoundary(outer, false);
        g_armor.armorEncodedFailed = true;
        g_armor.armorEncodedStatus = "Disabled: scene shader inside armor scope";
    }
}

bool PixelReplacementEligible()
{
    return !g_armor.armorEncodedActive && HdrPathRequested() &&
        g_referenceHealthy && g_gameplayReadyApplied.load(std::memory_order_acquire) &&
        ReferenceDisplayStillEligible();
}

void SelectPixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    DrawObservation& record, ID3D11PixelShader*& selectedShader)
{
    if (const auto* descriptor = FindShader(shader))
    {
        CloseArmorForSceneShader(context);
        record.currentPass = descriptor->pass;
        record.currentPassKnown = true;
        record.currentRequirement = descriptor->requirement;
        if (PixelReplacementEligible())
        {
            if (auto* replacement = FindReplacement(descriptor->pass))
                selectedShader = replacement;
        }
    }
    else
    {
        record.currentPassKnown = false;
        record.currentRequirement =
            PipelinePassRequirement::untracked;
    }
}

void STDMETHODCALLTYPE HookPsSetShader(
    ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classes, UINT classCount) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextPsSetShader == nullptr) return;
    if (context != g_rendererDevice.activeContext)
    {
        InvokeCapturedNext(record->nextPsSetShader, context, shader, classes, classCount);
        return;
    }
    auto* selectedShader = shader;
    try
    {
        SelectPixelShader(context, shader, *record, selectedShader);
        record->currentReplacementActive = selectedShader != shader;
        record->expectedBoundShader = selectedShader;
    }
    catch (...)
    {
        record->currentPassKnown = false;
        record->currentReplacementActive = false;
        record->expectedBoundShader = nullptr;
        record->currentRequirement = PipelinePassRequirement::untracked;
        RecordAbiException();
    }
    InvokeCapturedNext(record->nextPsSetShader, context, selectedShader,
        classes, classCount);
}

bool HasExpectedShaderBinding(ID3D11DeviceContext* context, const DrawObservation& record) noexcept
{
    return context != nullptr && record.currentPassKnown && record.currentReplacementActive &&
        record.expectedBoundShader != nullptr;
}

[[nodiscard]] bool ValidateExpectedShaderBinding(
    ID3D11DeviceContext* context,
    DrawObservation& record) noexcept
{
    if (!HasExpectedShaderBinding(context, record))
        return true;
    ComPtr<ID3D11PixelShader> actual;
    context->PSGetShader(&actual, nullptr, nullptr);
    if (actual.Get() == record.expectedBoundShader) return true;

    // A downstream owner changed a shader after Fallout4HDR selected its
    // replacement. Required-pass collisions disable this generation; an
    // optional collision disables only scene continuation for this draw.
    if (DecideUnknownShaderOutcome(record.currentRequirement) ==
        UnknownShaderOutcome::disableHdrGeneration)
    {
        g_referenceHealthy = false;
        RecordFailure(GraphicsObservationFailure::requiredHeartbeatMissing);
    }
    record.currentPassKnown = false;
    record.currentReplacementActive = false;
    record.expectedBoundShader = actual.Get();
    return false;
}

bool CanRedirectPresentation(ID3D11RenderTargetView* view) noexcept
{
    return view != nullptr && HdrPathRequested() && g_referenceHealthy &&
        g_gameplayReadyApplied.load(std::memory_order_acquire) &&
        g_scene.currentScene != nullptr && g_presentation.presentationProxyRtv &&
        g_presentation.presentationBackBuffer &&
        g_scene.currentSceneDomain != LinearColorDomain::unknown;
}

bool AcceptPresentationDomain() noexcept
{
    if (g_presentation.presentationProxyDomain != LinearColorDomain::unknown &&
        g_presentation.presentationProxyDomain != g_scene.currentSceneDomain)
    {
        g_evidence.colorDomainMismatchCount.fetch_add(1, std::memory_order_relaxed);
        RecordFailure(GraphicsObservationFailure::colorDomainMismatch);
        g_referenceHealthy = false;
        return false;
    }
    return true;
}

ID3D11RenderTargetView* PublishPresentationRedirect() noexcept
{
    g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
    g_presentation.presentationRedirectedThisFrame = true;
    g_evidence.presentationRedirectCount.fetch_add(1, std::memory_order_relaxed);
    return g_presentation.presentationProxyRtv.Get();
}

[[nodiscard]] ID3D11RenderTargetView* RedirectPresentationView(
    ID3D11RenderTargetView* view) noexcept
{
    if (!CanRedirectPresentation(view)) return view;

    for (const auto* known : g_presentation.presentationViews)
    {
        if (known == view)
        {
            if (!AcceptPresentationDomain()) return view;
            return PublishPresentationRedirect();
        }
    }

    // Existing game RTVs survive a display-generation reset even when the
    // proxy is recreated. Recover their exact backbuffer identity once.
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    view->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R10G10B10A2_UNORM)
        return view;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!RememberPresentationView(resource.Get(), view)) return view;
    return PublishPresentationRedirect();
}

using RenderTargetArray = std::array<ID3D11RenderTargetView*,
    D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>;

bool RedirectRenderTargetArray(UINT count, ID3D11RenderTargetView* const* views,
    RenderTargetArray& redirected) noexcept
{
    bool changed{};
    for (UINT index = 0; index < count; ++index)
    {
        redirected[index] = RedirectPresentationView(views[index]);
        changed |= redirected[index] != views[index];
    }
    return changed;
}

bool IsSinglePresentationTarget(UINT count, ID3D11RenderTargetView* const* views,
    const RenderTargetArray& redirected) noexcept
{
    return count == 1 && redirected[0] != views[0] &&
        redirected[0] == g_presentation.presentationProxyRtv.Get();
}

ID3D11RenderTargetView* RenderTargetAt(UINT count,
    ID3D11RenderTargetView* const* selected, UINT index) noexcept
{
    return count > index && selected != nullptr ? selected[index] : nullptr;
}

[[nodiscard]] ID3D11RenderTargetView* const* PrepareRenderTargets(
    DrawObservation& record, UINT count,
    ID3D11RenderTargetView* const* views,
    RenderTargetArray& redirected) noexcept
{
    auto* selected = views;
    record.currentSinglePresentationTarget = false;
    if (views != nullptr && count <= redirected.size())
    {
        if (RedirectRenderTargetArray(count, views, redirected))
            selected = redirected.data();
        record.currentSinglePresentationTarget =
            IsSinglePresentationTarget(count, views, redirected);
    }
    record.currentPrimaryRtv = RenderTargetAt(count, selected, 0);
    record.currentSecondaryRtv = RenderTargetAt(count, selected, 1);
    return selected;
}

void STDMETHODCALLTYPE HookOmSetRenderTargets(
    ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* depth) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextOmSetRenderTargets == nullptr) return;
    if (context != g_rendererDevice.activeContext)
    {
        InvokeCapturedNext(record->nextOmSetRenderTargets, context, count, views, depth);
        return;
    }
    RenderTargetArray redirected{};
    auto* selected = PrepareRenderTargets(*record, count, views, redirected);
    InvokeCapturedNext(record->nextOmSetRenderTargets, context, count,
        selected, depth);
}

struct RenderTargetsAndUavsRequest
{
    ID3D11DeviceContext* context;
    UINT rtvCount;
    ID3D11RenderTargetView* const* rtvs;
    ID3D11DepthStencilView* depth;
    UINT uavStart;
    UINT uavCount;
    ID3D11UnorderedAccessView* const* uavs;
    const UINT* initialCounts;
};

void BindRenderTargetsAndUavs(const RenderTargetsAndUavsRequest& request) noexcept
{
    const auto& [context, rtvCount, rtvs, depth, uavStart, uavCount, uavs, initialCounts] = request;
    auto* record = FindContext(context);
    if (record == nullptr ||
        record->nextOmSetRenderTargetsAndUavs == nullptr)
        return;
    if (context != g_rendererDevice.activeContext)
    {
        InvokeCapturedNext(record->nextOmSetRenderTargetsAndUavs, context,
            rtvCount, rtvs, depth, uavStart, uavCount, uavs, initialCounts);
        return;
    }
    RenderTargetArray redirected{};
    auto* selected = rtvs;
    if (rtvCount != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        selected = PrepareRenderTargets(*record, rtvCount, rtvs, redirected);
    InvokeCapturedNext(record->nextOmSetRenderTargetsAndUavs, context,
        rtvCount, selected, depth, uavStart, uavCount, uavs, initialCounts);
}


void STDMETHODCALLTYPE HookOmSetRenderTargetsAndUavs(
    ID3D11DeviceContext* context, UINT rtvCount,
    ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* depth, UINT uavStart, UINT uavCount,
    ID3D11UnorderedAccessView* const* uavs,
    const UINT* initialCounts) noexcept
{
    BindRenderTargetsAndUavs({context, rtvCount, rtvs, depth, uavStart, uavCount, uavs, initialCounts});
}

template <auto NextMember, class... Arguments>
void ForwardDrawCall(ID3D11DeviceContext* context, bool exactIndexedQuad,
    Arguments... arguments) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->*NextMember == nullptr) return;
    const auto next = record->*NextMember;
    auto draw = [=]() { InvokeCapturedNext(next, context, arguments...); };
    const DrawInvocation invocation{
        [](void* callable) { (*static_cast<decltype(draw)*>(callable))(); }, &draw};
    ExecuteObservedDraw(*record, context, exactIndexedQuad, invocation);
}

void STDMETHODCALLTYPE HookDraw(
    ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDraw>(context, false, vertexCount, startVertex);
}

void STDMETHODCALLTYPE HookDrawIndexed(
    ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDrawIndexed>(context, indexCount == 6, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(
    ID3D11DeviceContext* context, UINT indices, UINT instances,
    UINT startIndex, INT baseVertex, UINT startInstance) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDrawIndexedInstanced>(context, false,
        indices, instances, startIndex, baseVertex, startInstance);
}

void STDMETHODCALLTYPE HookDrawInstanced(
    ID3D11DeviceContext* context, UINT vertices, UINT instances,
    UINT startVertex, UINT startInstance) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDrawInstanced>(context, false,
        vertices, instances, startVertex, startInstance);
}

} // namespace fo4hdr
