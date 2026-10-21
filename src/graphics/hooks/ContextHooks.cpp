// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
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
#include "../FrameTrace.hpp"
#include "../LiveSettings.hpp"
#include "../PresentFrame.hpp"
#include "../RendererLifetime.hpp"
#include "../ShaderRegistry.hpp"
#include "../passes/ArmorCompositionPass.hpp"
#include "../passes/ObservedDraw.hpp"
#include "../resources/ArmorCompositionResources.hpp"
#include "../resources/PresentationResources.hpp"
#include "../resources/ReplacementShaders.hpp"
#include "../resources/SceneResources.hpp"
#include "../SharedUiFrame.hpp"
#include "../SharedSdrFrame.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace fo4hdr {

using Microsoft::WRL::ComPtr;

template <auto NextMember>
using ContextNext = std::remove_cvref_t<decltype(std::declval<ContextRecord&>().*NextMember)>;

namespace {
thread_local ID3D11DeviceContext* ownedPresentationContext{};
thread_local bool insideLowerPresent{};

bool ObserveGameContext(ID3D11DeviceContext* context) noexcept
{
    return context == g_rendererDevice.activeContext && context != ownedPresentationContext &&
        !insideLowerPresent;
}
} // namespace

ScopedLowerPresent::ScopedLowerPresent() noexcept : previous_(insideLowerPresent)
{
    insideLowerPresent = true;
}

ScopedLowerPresent::~ScopedLowerPresent() noexcept
{
    insideLowerPresent = previous_;
}

bool InsideLowerPresent() noexcept
{
    return insideLowerPresent;
}

ScopedPresentationContext::ScopedPresentationContext(
    ID3D11DeviceContext* context, PresentationScope scope) noexcept
{
    if (ownedPresentationContext || context != g_rendererDevice.activeContext) return;
    const auto* record = FindContext(context);
    if (!record || !record->immediate) return;
    if (scope == PresentationScope::midFrame && !record->computeQueries.CanDispatch()) return;
    context_ = context;
    ownedPresentationContext = context;
}

ScopedPresentationContext::~ScopedPresentationContext() noexcept
{
    if (context_) ownedPresentationContext = nullptr;
}

ULONG STDMETHODCALLTYPE HookContextRelease(
    ID3D11DeviceContext* context) noexcept
{
    return InvokeNoexceptBoundary<ULONG>(1, [=]() {
        auto* record = FindContext(context);
        if (record == nullptr || record->nextRelease == nullptr)
            return ForwardUnrecorded<ContextRelease>(1UL, HookAt(kRelease, &HookContextRelease),
                context);
        const auto generation = record->identity.Generation();
        const auto remaining = InvokeCapturedNext(record->nextRelease, context);
        if (remaining == 0)
        {
            record->nativePixelShader.Reset();
            RetireFrameTrace();
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
    // A null Begin opens no scope.
    if (asynchronous == nullptr) return;
    ComPtr<ID3D11Query> query;
    if (FAILED(asynchronous->QueryInterface(IID_PPV_ARGS(&query))))
    {
        ComPtr<ID3D11Counter> counter;
        if (FAILED(asynchronous->QueryInterface(IID_PPV_ARGS(&counter)))) queries.unknown = true;
        return;
    }
    D3D11_QUERY_DESC description{};
    query->GetDesc(&description);
    if (IsEndOnlyQuery(description.Query)) return;
    if (IsComputeSensitiveQuery(description.Query))
        queries.Begin(asynchronous);
    else if (!IsProfilingQuery(description.Query) &&
        !IsStreamOutputQuery(description.Query))
        queries.unknown = true;
}

void STDMETHODCALLTYPE HookBegin(
    ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextBegin == nullptr)
        return ForwardUnrecordedCall<BeginQuery>(HookAt(kContextBegin, &HookBegin),
            context, asynchronous);
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
    if (record == nullptr || record->nextEnd == nullptr)
        return ForwardUnrecordedCall<EndQuery>(HookAt(kContextEnd, &HookEnd),
            context, asynchronous);
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

void ResetCommandListObservation(DrawObservation& observation, bool retained) noexcept
{
    if (retained) return;
    // FALSE or a failed replay cannot retain pre-list shader/target observations.
    observation.currentPrimaryRtv = nullptr;
    observation.currentSecondaryRtv = nullptr;
    observation.currentSinglePresentationTarget = false;
    observation.currentPass = {};
    observation.currentPassKnown = false;
    observation.currentReplacementActive = false;
    observation.expectedBoundShader = nullptr;
    observation.nativePixelShader.Reset();
    observation.currentRequirement = PipelinePassRequirement::untracked;
}

void ObserveCopyResources(ID3D11DeviceContext* context,
    ID3D11Resource* destination, ID3D11Resource* source) noexcept;

void STDMETHODCALLTYPE HookExecuteCommandList(
    ID3D11DeviceContext* context, ID3D11CommandList* commands,
    BOOL restoreState) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextExecuteCommandList == nullptr)
        return ForwardUnrecordedCall<ExecuteCommandList>(
            HookAt(kContextExecuteCommandList, &HookExecuteCommandList),
            context, commands, restoreState);
    // Opaque replay loses quality proof; restore armor before exposing its temporary codes.
    ObserveCopyResources(context, g_presentation.presentationProxy.Get(), nullptr);
    if (context != ownedPresentationContext)
        TraceFrameCommandList(context, *record, commands, restoreState);
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
    ResetCommandListObservation(*record, restoreState && completed);
}

bool ArmorScopeOnPresentThread(ID3D11DeviceContext* context) noexcept
{
    return g_armor.armorEncodedActive && context == g_rendererDevice.activeContext &&
        GetCurrentThreadId() == g_observedPresentThread.load(std::memory_order_acquire);
}

void CloseConflictingArmorScope(ID3D11DeviceContext* context)
{
    if (ArmorScopeOnPresentThread(context))
    {
        ArmorUiScopeToken outer{};
        outer.serial = g_armor.armorEncodedScope;
        ArmorEncodedBoundary(outer, false);
        if (!g_armor.armorEncodedSharedSrgb) g_armor.armorEncodedFailed = true;
        if (g_referenceHealthy)
            g_armor.armorEncodedStatus = "Rejected frame: conflicting operation inside armor scope";
    }
}

bool PixelReplacementEligible()
{
    if (SharedUiFrameEncoded()) return false;
    return !g_armor.armorEncodedActive && HdrPathRequested() &&
        g_referenceHealthy && g_productionHealthy &&
        g_gameplayReadyApplied.load(std::memory_order_acquire) &&
        ReferenceDisplayStillEligible();
}

void SelectPixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    DrawObservation& record, ID3D11PixelShader*& selectedShader)
{
    const auto* descriptor = FindShader(shader);
    if (!descriptor)
    {
        record.currentPassKnown = false;
        record.currentRequirement =
            PipelinePassRequirement::untracked;
        return;
    }
    if (!g_armor.armorEncodedSharedSrgb) CloseConflictingArmorScope(context);
    record.currentPass = descriptor->pass;
    record.currentPassKnown = true;
    record.currentRequirement = descriptor->requirement;
    if (PixelReplacementEligible())
    {
        if (auto* replacement = FindReplacement(descriptor->pass))
            selectedShader = replacement;
    }
}

void STDMETHODCALLTYPE HookPsSetShader(
    ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classes, UINT classCount) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->nextPsSetShader == nullptr)
        return ForwardUnrecordedCall<PsSetShader>(HookAt(kContextPsSetShader, &HookPsSetShader),
            context, shader, classes, classCount);
    if (!ObserveGameContext(context))
    {
        InvokeCapturedNext(record->nextPsSetShader, context, shader, classes, classCount);
        return;
    }
    auto* selectedShader = shader;
    try
    {
        ApplyGraphicsLifecycleBeforeDraw(context);
        record->currentPassKnown = false;
        record->currentRequirement = PipelinePassRequirement::untracked;
        if (classCount == 0) SelectPixelShader(context, shader, *record, selectedShader);
        record->currentReplacementActive = selectedShader != shader;
        record->expectedBoundShader = selectedShader;
        record->nativePixelShader = record->currentReplacementActive ? shader : nullptr;
    }
    catch (...)
    {
        record->currentPassKnown = false;
        record->currentReplacementActive = false;
        record->expectedBoundShader = nullptr;
        record->nativePixelShader.Reset();
        record->currentRequirement = PipelinePassRequirement::untracked;
        selectedShader = shader;
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

bool RestoreNativePixelShader(ID3D11DeviceContext* context, DrawObservation& record) noexcept
{
    if (!record.nativePixelShader || !HasOriginalPixelShaderSetter(context)) return false;
    const auto native = std::move(record.nativePixelShader);
    BindOriginalPixelShader(context, native.Get());
    ComPtr<ID3D11PixelShader> actual;
    context->PSGetShader(&actual, nullptr, nullptr);
    record.currentReplacementActive = false;
    record.expectedBoundShader = actual.Get();
    if (actual.Get() == native.Get()) return true;
    record.currentPassKnown = false;
    record.currentRequirement = PipelinePassRequirement::untracked;
    return false;
}

[[nodiscard]] bool ValidateExpectedShaderBinding(
    ID3D11DeviceContext* context,
    DrawObservation& record) noexcept
{
    if (!HasExpectedShaderBinding(context, record))
        return true;
    ComPtr<ID3D11PixelShader> actual;
    context->PSGetShader(&actual, nullptr, nullptr);
    if (actual.Get() == record.expectedBoundShader)
        return g_productionHealthy || RestoreNativePixelShader(context, record);

    // A downstream owner changed a shader after FO4Isotope selected its
    // replacement. Required-pass collisions disable production for this generation; an
    // optional collision disables only scene continuation for this draw.
    if (DecideUnknownShaderOutcome(record.currentRequirement) ==
        UnknownShaderOutcome::disableHdrGeneration)
    {
        g_productionHealthy = false;
        RecordFailure(GraphicsObservationFailure::requiredHeartbeatMissing);
    }
    record.currentPassKnown = false;
    record.currentReplacementActive = false;
    record.expectedBoundShader = actual.Get();
    record.nativePixelShader.Reset();
    return false;
}

bool CanRedirectPresentation(ID3D11RenderTargetView* view) noexcept
{
    const std::array ready{view != nullptr, HdrPathRequested(), g_referenceHealthy.load(),
        g_productionHealthy.load(), g_gameplayReadyApplied.load(std::memory_order_acquire),
        g_scene.currentScene != nullptr, g_presentation.presentationProxyRtv != nullptr,
        g_presentation.presentationBackBuffer != nullptr,
        g_scene.currentSceneDomain != LinearColorDomain::unknown};
    return std::ranges::all_of(ready, [](bool value) { return value; });
}

// The armor bridge declares its gamma 2.2 interval; only its decode restores the label.
bool ArmorBridgeEncoded() noexcept
{
    return g_armor.armorEncodedActive &&
        g_presentation.presentationProxyDomain == LinearColorDomain::encodedGamma22Bt709UiRelative;
}

bool AcceptPresentationDomain() noexcept
{
    if (ArmorBridgeEncoded()) return true;
    if (g_presentation.presentationProxyDomain != LinearColorDomain::unknown &&
        g_presentation.presentationProxyDomain != g_scene.currentSceneDomain)
    {
        g_evidence.colorDomainMismatchCount.fetch_add(1, std::memory_order_relaxed);
        RecordFailure(GraphicsObservationFailure::colorDomainMismatch);
        return false;
    }
    return true;
}

ID3D11RenderTargetView* PublishPresentationRedirect(ID3D11RenderTargetView* view) noexcept
{
    if (!ArmorBridgeEncoded()) g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
    g_presentation.presentationRedirectedThisFrame = true;
    g_evidence.presentationRedirectCount.fetch_add(1, std::memory_order_relaxed);
    return view;
}

// Resource identity is proven at every bind; a cached raw view pointer can be
// reused by an unrelated view after release. The facade game never sees the
// native backbuffer, so only foreign work can bind it there.
bool IsRedirectedBackBuffer(ID3D11RenderTargetView* view) noexcept
{
    return !ApplicationPresentationFacadeActive() && IsPresentationBackBufferTarget(view);
}

[[nodiscard]] ID3D11RenderTargetView* RedirectPresentationView(
    ID3D11RenderTargetView* view) noexcept
{
    if (!CanRedirectPresentation(view)) return view;

    const bool exactProxy = IsPresentationProxyTarget(view);
    if (!exactProxy && !IsRedirectedBackBuffer(view)) return view;
    if (!AcceptPresentationDomain()) return view;
    return PublishPresentationRedirect(
        exactProxy ? view : g_presentation.presentationProxyRtv.Get());
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

bool IsSinglePresentationTarget(UINT count, const RenderTargetArray& redirected) noexcept
{
    return count == 1 && IsPresentationProxyTarget(redirected[0]);
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
            IsSinglePresentationTarget(count, redirected);
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
    if (record == nullptr || record->nextOmSetRenderTargets == nullptr)
        return ForwardUnrecordedCall<OmSetRenderTargets>(
            HookAt(kContextOmSetRenderTargets, &HookOmSetRenderTargets),
            context, count, views, depth);
    if (!ObserveGameContext(context))
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
        return ForwardUnrecordedCall<OmSetRenderTargetsAndUavs>(
            HookAt(kContextOmSetRenderTargetsAndUavs, &HookOmSetRenderTargetsAndUavs), context,
            rtvCount, rtvs, depth, uavStart, uavCount, uavs, initialCounts);
    if (!ObserveGameContext(context))
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

template <auto NextMember, std::size_t Slot, auto Hook, class... Arguments>
void ForwardDrawCall(ID3D11DeviceContext* context, FrameTraceKind kind,
    Arguments... arguments) noexcept
{
    auto* record = FindContext(context);
    if (record == nullptr || record->*NextMember == nullptr)
        return ForwardUnrecordedCall<ContextNext<NextMember>>(HookAt(Slot, Hook),
            context, arguments...);
    const auto next = record->*NextMember;
    if (context == ownedPresentationContext || insideLowerPresent)
    {
        InvokeCapturedNext(next, context, arguments...);
        return;
    }
    auto draw = [=]() {
        TraceFrameDraw(context, *record, kind, {static_cast<std::uint32_t>(arguments)...});
        InvokeCapturedNext(next, context, arguments...);
    };
    const FrameTraceArguments args{static_cast<std::uint32_t>(arguments)...};
    const DrawInvocation invocation{
        [](void* callable) { (*static_cast<decltype(draw)*>(callable))(); }, &draw,
        kind == FrameTraceKind::drawIndexed && args[0] == 3 && args[1] == 0 && args[2] == 0};
    ExecuteObservedDraw(*record, context, invocation);
}

void STDMETHODCALLTYPE HookDraw(
    ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDraw, kContextDraw, &HookDraw>(
        context, FrameTraceKind::draw, vertexCount, startVertex);
}

void STDMETHODCALLTYPE HookDrawIndexed(
    ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDrawIndexed, kContextDrawIndexed, &HookDrawIndexed>(context,
        FrameTraceKind::drawIndexed, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(
    ID3D11DeviceContext* context, UINT indices, UINT instances,
    UINT startIndex, INT baseVertex, UINT startInstance) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDrawIndexedInstanced, kContextDrawIndexedInstanced,
        &HookDrawIndexedInstanced>(context, FrameTraceKind::drawIndexedInstanced,
        indices, instances, startIndex, baseVertex, startInstance);
}

void STDMETHODCALLTYPE HookDrawInstanced(
    ID3D11DeviceContext* context, UINT vertices, UINT instances,
    UINT startVertex, UINT startInstance) noexcept
{
    ForwardDrawCall<&ContextRecord::nextDrawInstanced, kContextDrawInstanced,
        &HookDrawInstanced>(context, FrameTraceKind::drawInstanced,
        vertices, instances, startVertex, startInstance);
}

void ObserveCopyResources(ID3D11DeviceContext* context,
    ID3D11Resource* destination, ID3D11Resource* source) noexcept
{
    if (!ObserveGameContext(context)) return;
    if (ObserveSharedUiCopy(destination, source)) CloseConflictingArmorScope(context);
}

void STDMETHODCALLTYPE HookCopyResource(ID3D11DeviceContext* context,
    ID3D11Resource* destination, ID3D11Resource* source) noexcept
{
    auto* record = FindContext(context);
    if (!record || !record->nextCopyResource)
        return ForwardUnrecordedCall<CopyResource>(HookAt(kContextCopyResource, &HookCopyResource),
            context, destination, source);
    ObserveCopyResources(context, destination, source);
    if (context != ownedPresentationContext)
        TraceFrameCopy(context, *record, {FrameTraceKind::copy, destination, source});
    try { InvokeCapturedNext(record->nextCopyResource, context, destination, source); }
    catch (...) { RecordAbiException(); }
}

void STDMETHODCALLTYPE HookCopySubresourceRegion(ID3D11DeviceContext* context,
    ID3D11Resource* destination, UINT destinationSubresource, UINT x, UINT y, UINT z,
    ID3D11Resource* source, UINT sourceSubresource, const D3D11_BOX* box) noexcept
{
    auto* record = FindContext(context);
    if (!record || !record->nextCopySubresourceRegion)
        return ForwardUnrecordedCall<CopySubresourceRegion>(
            HookAt(kContextCopySubresourceRegion, &HookCopySubresourceRegion), context,
            destination, destinationSubresource, x, y, z, source, sourceSubresource, box);
    ObserveCopyResources(context, destination, source);
    if (context != ownedPresentationContext && FrameTraceRequested())
    {
        FrameTraceArguments args{destinationSubresource, x, y, z, sourceSubresource, box ? 1U : 0U};
        if (box) { args[6] = box->left; args[7] = box->top; args[8] = box->front;
            args[9] = box->right; args[10] = box->bottom; args[11] = box->back; }
        TraceFrameCopy(context, *record, {FrameTraceKind::copyRegion, destination, source, args});
    }
    try {
        InvokeCapturedNext(record->nextCopySubresourceRegion, context,
            destination, destinationSubresource, x, y, z, source, sourceSubresource, box);
    }
    catch (...) { RecordAbiException(); }
}

void STDMETHODCALLTYPE HookClearRenderTargetView(ID3D11DeviceContext* context,
    ID3D11RenderTargetView* target, const FLOAT* color) noexcept
{
    auto* record = FindContext(context);
    if (!record || !record->nextClearRenderTargetView)
        return ForwardUnrecordedCall<ClearRenderTargetView>(
            HookAt(kContextClearRenderTargetView, &HookClearRenderTargetView),
            context, target, color);
    if (ObserveGameContext(context)) ObserveSharedUiClear(target);
    if (context != ownedPresentationContext) TraceFrameClear(context, *record, target, color);
    try { InvokeCapturedNext(record->nextClearRenderTargetView, context, target, color); }
    catch (...) { RecordAbiException(); }
}

void ObserveUnorderedAccessWrite(ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* target) noexcept
{
    if (!target) return;
    if (!ObserveGameContext(context) || !ApplicationPresentationFacadeActive()) return;
    ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    ObserveCopyResources(context, resource.Get(), nullptr);
}

void ObserveComputeWrites(ID3D11DeviceContext* context) noexcept
{
    if (!ObserveGameContext(context) || !ApplicationPresentationFacadeActive()) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
        D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> targets{};
    context->CSGetUnorderedAccessViews(0, count, targets.data());
    for (auto* target : targets)
    {
        ObserveUnorderedAccessWrite(context, target);
        if (target) target->Release();
    }
}

template <auto NextMember, std::size_t Slot, auto Hook, class... Arguments>
void ForwardComputeCall(ID3D11DeviceContext* context, Arguments... arguments) noexcept
{
    auto* record = FindContext(context);
    if (!record || !(record->*NextMember))
        return ForwardUnrecordedCall<ContextNext<NextMember>>(HookAt(Slot, Hook),
            context, arguments...);
    ObserveComputeWrites(context);
    try { InvokeCapturedNext(record->*NextMember, context, arguments...); }
    catch (...) { RecordAbiException(); }
}

void STDMETHODCALLTYPE HookDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z) noexcept
{
    ForwardComputeCall<&ContextRecord::nextDispatch, kContextDispatch, &HookDispatch>(
        context, x, y, z);
}

void STDMETHODCALLTYPE HookDispatchIndirect(ID3D11DeviceContext* context,
    ID3D11Buffer* arguments, UINT offset) noexcept
{
    ForwardComputeCall<&ContextRecord::nextDispatchIndirect, kContextDispatchIndirect,
        &HookDispatchIndirect>(context, arguments, offset);
}

template <auto NextMember, std::size_t Slot, auto Hook, class Value>
void ForwardUnorderedAccessClear(ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* target, const Value* values) noexcept
{
    auto* record = FindContext(context);
    if (!record || !(record->*NextMember))
        return ForwardUnrecordedCall<ContextNext<NextMember>>(HookAt(Slot, Hook),
            context, target, values);
    ObserveUnorderedAccessWrite(context, target);
    try { InvokeCapturedNext(record->*NextMember, context, target, values); }
    catch (...) { RecordAbiException(); }
}

void STDMETHODCALLTYPE HookClearUnorderedAccessViewUint(ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* target, const UINT* values) noexcept
{
    ForwardUnorderedAccessClear<&ContextRecord::nextClearUnorderedAccessViewUint,
        kContextClearUnorderedAccessViewUint, &HookClearUnorderedAccessViewUint>(
        context, target, values);
}

void STDMETHODCALLTYPE HookClearUnorderedAccessViewFloat(ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* target, const FLOAT* values) noexcept
{
    ForwardUnorderedAccessClear<&ContextRecord::nextClearUnorderedAccessViewFloat,
        kContextClearUnorderedAccessViewFloat, &HookClearUnorderedAccessViewFloat>(
        context, target, values);
}

} // namespace fo4hdr
