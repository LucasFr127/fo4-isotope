// SPDX-License-Identifier: GPL-3.0-or-later

#include "ContextHooks.hpp"
#include "HookRecords.hpp"
#include "../HdrRenderer.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <type_traits>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

enum class QueryKind
{
    unknown,
    scoped,
    endOnly
};

QueryKind ClassifyQuery(D3D11_QUERY kind) noexcept
{
    if (kind == D3D11_QUERY_EVENT || kind == D3D11_QUERY_TIMESTAMP)
        return QueryKind::endOnly;
    constexpr std::array scoped{
        D3D11_QUERY_OCCLUSION, D3D11_QUERY_TIMESTAMP_DISJOINT, D3D11_QUERY_PIPELINE_STATISTICS,
        D3D11_QUERY_OCCLUSION_PREDICATE, D3D11_QUERY_SO_STATISTICS,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE, D3D11_QUERY_SO_STATISTICS_STREAM0,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0, D3D11_QUERY_SO_STATISTICS_STREAM1,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1, D3D11_QUERY_SO_STATISTICS_STREAM2,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2, D3D11_QUERY_SO_STATISTICS_STREAM3,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3};
    return std::find(scoped.begin(), scoped.end(), kind) != scoped.end()
        ? QueryKind::scoped : QueryKind::unknown;
}

QueryKind Classify(ID3D11Asynchronous* asynchronous) noexcept
{
    if (asynchronous == nullptr) return QueryKind::unknown;
    ComPtr<ID3D11Query> query;
    if (FAILED(asynchronous->QueryInterface(IID_PPV_ARGS(&query)))) return QueryKind::unknown;
    if (!query) return QueryKind::unknown;
    D3D11_QUERY_DESC description{};
    query->GetDesc(&description);
    return ClassifyQuery(description.Query);
}

template <class Call, class... Arguments>
bool CallCaptured(Call next, Arguments... arguments) noexcept
{
    if (next == nullptr) return false;
    try { next(arguments...); return true; }
    catch (...) { return false; }
}

class ContextObservationScope
{
public:
    ContextObservationScope(std::atomic_flag& flag, bool entered) noexcept
        : flag_(flag), entered_(entered) {}
    ~ContextObservationScope() noexcept
    {
        if (entered_) flag_.clear(std::memory_order_release);
    }
    ContextObservationScope(const ContextObservationScope&) = delete;
    ContextObservationScope& operator=(const ContextObservationScope&) = delete;

private:
    std::atomic_flag& flag_;
    bool entered_;
};

bool SupportsContextWindows(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    if (!context1) return false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
            &options, sizeof(options)))) return false;
    return options.ConstantBufferOffsetting;
}

bool IsDeviceImmediateContext(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<IUnknown> suppliedIdentity, immediateIdentity;
    device->GetImmediateContext(&immediate);
    if (!immediate) return false;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&suppliedIdentity)))) return false;
    if (FAILED(immediate->QueryInterface(IID_PPV_ARGS(&immediateIdentity)))) return false;
    return suppliedIdentity && suppliedIdentity.Get() == immediateIdentity.Get();
}

bool QualifiedContext(ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return false;
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    if (!IsDeviceImmediateContext(device.Get(), context)) return false;
    return SupportsContextWindows(device.Get(), context);
}

}  // namespace

bool ContextQueryObserver::CompleteFreshCoverage(ID3D11DeviceContext* context,
    std::uint64_t generation) noexcept
{
    if (context != context_ || generation != generation_) return false;
    if (ownerThread_ != std::this_thread::get_id()) return false;
    if (lostCoverage_.load(std::memory_order_acquire)) return false;
    if (observing_.test(std::memory_order_acquire)) return false;
    if (queries_.executionDepth != 0) return false;
    if (!std::all_of(queries_.active.begin(), queries_.active.end(),
            [](const void* key) { return key == nullptr; })) return false;
    // Only the installer can reach this after its complete pre-escape transaction.
    queries_.unknown = false;
    return queries_.CanDispatch();
}

ContextQueryPreparation ContextQueryObserver::Prepare(ID3D11DeviceContext* context,
    std::uint64_t generation, ContextQueryPredecessors next, ScenePassRenderer* renderer) noexcept
{
    if (context_ || generation == 0) return ContextQueryPreparation::invalidContext;
    if (!QualifiedContext(context)) return ContextQueryPreparation::invalidContext;
    if (renderer != nullptr)
    {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!renderer->MatchesContext(device.Get(), context))
            return ContextQueryPreparation::invalidContext;
    }
    context_ = context;
    renderer_ = renderer;
    generation_ = generation;
    ownerThread_ = std::this_thread::get_id();
    next_ = next;
    // Positive Context1 facts do not prove that every Begin/End was observed.
    queries_.unknown = true;
    return ContextQueryPreparation::coverageUnobserved;
}

bool ContextQueryObserver::Retire() noexcept
{
    if (context_ && ownerThread_ != std::this_thread::get_id()) return false;
    if (observing_.test(std::memory_order_acquire)) return false;
    context_ = nullptr;
    renderer_ = nullptr;
    generation_ = 0;
    next_ = {};
    queries_ = {};
    lostCoverage_.store(false, std::memory_order_relaxed);
    return true;
}

bool ContextQueryObserver::CanDispatch() const noexcept
{
    if (ownerThread_ != std::this_thread::get_id()) return false;
    if (!context_ || generation_ == 0) return false;
    if (lostCoverage_.load(std::memory_order_acquire)) return false;
    if (observing_.test(std::memory_order_acquire)) return false;
    return queries_.CanDispatch();
}

bool ContextQueryObserver::Enter(ID3D11DeviceContext* context) noexcept
{
    if (context != context_ || ownerThread_ != std::this_thread::get_id())
    {
        lostCoverage_.store(true, std::memory_order_release);
        return false;
    }
    if (observing_.test_and_set(std::memory_order_acquire))
    {
        lostCoverage_.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void ContextQueryObserver::ObserveBegin(ID3D11Asynchronous* asynchronous) noexcept
{
    if (Classify(asynchronous) != QueryKind::scoped) { queries_.unknown = true; return; }
    queries_.Begin(asynchronous);
}

void ContextQueryObserver::ObserveEnd(ID3D11Asynchronous* asynchronous) noexcept
{
    const auto kind = Classify(asynchronous);
    if (kind == QueryKind::unknown) { queries_.unknown = true; return; }
    if (kind == QueryKind::scoped) queries_.End(asynchronous);
}

void ContextQueryObserver::Begin(ID3D11DeviceContext* context,
    ID3D11Asynchronous* asynchronous) noexcept
{
    const bool observing = Enter(context);
    const ContextObservationScope observation{observing_, observing};
    if (observing) ObserveBegin(asynchronous);
    if (!CallCaptured(next_.begin, context, asynchronous))
        lostCoverage_.store(true, std::memory_order_release);
}

void ContextQueryObserver::End(ID3D11DeviceContext* context,
    ID3D11Asynchronous* asynchronous) noexcept
{
    const bool observing = Enter(context);
    const ContextObservationScope observation{observing_, observing};
    const bool completed = CallCaptured(next_.end, context, asynchronous);
    if (!completed) lostCoverage_.store(true, std::memory_order_release);
    // Downstream End callbacks still execute inside the active query interval.
    if (observing && completed) ObserveEnd(asynchronous);
}

void ContextQueryObserver::FinishCommandList(bool entered, bool completed,
    BOOL restoreState) noexcept
{
    if (entered) queries_.LeaveCommandList();
    if (restoreState && completed) return;
    // FALSE resets bindings; retained scene publication must not outlive that reset.
    if (renderer_ != nullptr) static_cast<void>(renderer_->EndFrame());
}

void ContextQueryObserver::ExecuteCommandList(ID3D11DeviceContext* context,
    ID3D11CommandList* commands, BOOL restoreState) noexcept
{
    const bool observing = Enter(context);
    const ContextObservationScope observation{observing_, observing};
    const bool entered = observing && queries_.EnterCommandList();
    if (observing && commands == nullptr) queries_.unknown = true;
    const bool completed = CallCaptured(next_.executeCommandList, context, commands, restoreState);
    if (!completed) lostCoverage_.store(true, std::memory_order_release);
    if (observing) FinishCommandList(entered, completed, restoreState);
}

namespace {

bool EnterRouting(ContextHookRecord& context) noexcept
{
    if (GetCurrentThreadId() != context.thread) { RevokeNativeRouting(); return false; }
    if (context.internalWork) return false;
    if (context.busy.test_and_set(std::memory_order_acquire)) { RevokeNativeRouting(); return false; }
    if (NativeRoutingOwned()) return true;
    RevokeNativeRouting();
    context.busy.clear(std::memory_order_release);
    return false;
}

}  // namespace

NativeRoutingScope::NativeRoutingScope(IUnknown* object) noexcept
{
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const auto* device = g_deviceHooks.load(std::memory_order_acquire);
    if (context == nullptr || device == nullptr) return;
    if (object != context->owned && object != device->owned) return;
    if (!context->admitted.load(std::memory_order_acquire)) return;
    if (!EnterRouting(*context)) return;
    record_ = context;
}

NativeRoutingScope::~NativeRoutingScope() noexcept
{
    if (record_ != nullptr) record_->busy.clear(std::memory_order_release);
}

namespace {

class InternalRoutingWork
{
public:
    explicit InternalRoutingWork(ContextHookRecord& record) noexcept : record_(record)
    {
        record_.internalWork = true;
    }
    ~InternalRoutingWork() noexcept { record_.internalWork = false; }
    InternalRoutingWork(const InternalRoutingWork&) = delete;
    InternalRoutingWork& operator=(const InternalRoutingWork&) = delete;

private:
    ContextHookRecord& record_;
};

class NativeCallbackWork
{
public:
    explicit NativeCallbackWork(ContextHookRecord* record) noexcept : record_(record)
    {
        if (record_ == nullptr) return;
        previous_ = record_->internalWork;
        record_->internalWork = false;
    }
    ~NativeCallbackWork() noexcept
    {
        if (record_ != nullptr) record_->internalWork = previous_;
    }
    NativeCallbackWork(const NativeCallbackWork&) = delete;
    NativeCallbackWork& operator=(const NativeCallbackWork&) = delete;

private:
    ContextHookRecord* record_{};
    bool previous_{};
};

ContextHookRecord* OwnedQueryContext(ContextHookRecord& record,
    ID3D11DeviceContext* context) noexcept
{
    if (context != record.owned) return nullptr;
    return GetCurrentThreadId() == record.thread ? &record : nullptr;
}

template <class Call, class... Arguments>
void ForwardContext(Call next, ID3D11DeviceContext* context, Arguments... arguments) noexcept
{
    if (CallCaptured(next, context, arguments...)) return;
    const auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record != nullptr && context == record->owned) RevokeNativeRouting();
}

void ObserveFiniteDraw(ContextHookRecord* record, NativeUiDrawKind kind,
    FiniteDrawInvocation invocation) noexcept
{
    if (record == nullptr) return;
    if (!record->queries.CanDispatch()) return;
    const InternalRoutingWork internal{*record};
    const auto captured = record->uiCapture.ExecuteObserved(record->renderer.FrameGeneration(), kind, invocation);
    if (!NativeRoutingOwned()) RevokeNativeRouting();
    if (!record->queries.CanDispatch()) { record->uiCapture.CloseFrame(); return; }
    if (captured == SceneDrawOutcome::callbackFailed) { RevokeNativeRouting(); return; }
    if (captured == SceneDrawOutcome::replaced) return;
    // Invalid scene handles preserve the native draw until actual storage is admitted.
    const auto result = record->renderer.ExecuteObserved({}, {}, invocation);
    if (result == SceneDrawOutcome::callbackFailed) RevokeNativeRouting();
}

template <class Call>
void InvokeFiniteDraw(Call& draw, bool admitted) noexcept
{
    try { draw(); }
    catch (...) { if (admitted) RevokeNativeRouting(); }
}

template <auto Next, class... Arguments>
void ForwardFiniteDraw(ID3D11DeviceContext* context, NativeUiDrawKind kind, Arguments... arguments) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const auto next = record->*Next;
    NativeRoutingScope scope{context};
    bool invoked{};
    auto draw = [&]() {
        const NativeCallbackWork native{scope.Record()};
        invoked = true;
        next(context, arguments...);
    };
    const FiniteDrawInvocation invocation{
        [](ID3D11DeviceContext*, void* payload) { (*static_cast<decltype(draw)*>(payload))(); }, &draw};
    ObserveFiniteDraw(scope.Record(), kind, invocation);
    if (!invoked) InvokeFiniteDraw(draw, scope.Record() != nullptr);
}

}  // namespace

void STDMETHODCALLTYPE HookBegin(ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const NativeCallbackWork native{OwnedQueryContext(*record, context)};
    const NativeRoutingScope scope{context};
    if (auto* admitted = scope.Record()) admitted->queries.Begin(context, asynchronous);
    else ForwardContext(record->nextBegin, context, asynchronous);
}

void STDMETHODCALLTYPE HookEnd(ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const NativeCallbackWork native{OwnedQueryContext(*record, context)};
    const NativeRoutingScope scope{context};
    if (auto* admitted = scope.Record()) admitted->queries.End(context, asynchronous);
    else ForwardContext(record->nextEnd, context, asynchronous);
}

void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* context,
    ID3D11CommandList* commands, BOOL restoreState) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const NativeCallbackWork native{OwnedQueryContext(*record, context)};
    const NativeRoutingScope scope{context};
    if (auto* admitted = scope.Record()) admitted->queries.ExecuteCommandList(context, commands, restoreState);
    else ForwardContext(record->nextExecuteCommandList, context, commands, restoreState);
}

void STDMETHODCALLTYPE HookPsSetShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classes, UINT classCount) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const NativeRoutingScope scope{context};
    // The draw consumer rechecks the actual shader and class tuple; retain no duplicate cache.
    ForwardContext(record->nextPsSetShader, context, shader, classes, classCount);
}

void STDMETHODCALLTYPE HookOmSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const NativeRoutingScope scope{context};
    ForwardContext(record->nextOmSetRenderTargets, context, count, targets, depth);
}

void STDMETHODCALLTYPE HookOmSetRenderTargetsAndUavs(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth, UINT firstUav, UINT uavCount,
    ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    const NativeRoutingScope scope{context};
    ForwardContext(record->nextOmSetRenderTargetsAndUavs, context, count, targets, depth,
        firstUav, uavCount, uavs, initialCounts);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertices, UINT start) noexcept
{
    ForwardFiniteDraw<&ContextHookRecord::nextDraw>(context, NativeUiDrawKind::finite, vertices, start);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indices, UINT start,
    INT baseVertex) noexcept
{
    const auto kind = indices == 6 ? NativeUiDrawKind::indexedSix : NativeUiDrawKind::finite;
    ForwardFiniteDraw<&ContextHookRecord::nextDrawIndexed>(context, kind, indices, start, baseVertex);
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* context, UINT vertices, UINT instances,
    UINT start, UINT firstInstance) noexcept
{
    ForwardFiniteDraw<&ContextHookRecord::nextDrawInstanced>(context, NativeUiDrawKind::finite,
        vertices, instances, start, firstInstance);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* context, UINT indices,
    UINT instances, UINT start, INT baseVertex, UINT firstInstance) noexcept
{
    ForwardFiniteDraw<&ContextHookRecord::nextDrawIndexedInstanced>(context, NativeUiDrawKind::finite, indices, instances,
        start, baseVertex, firstInstance);
}

static_assert(std::is_convertible_v<decltype(&HookBegin), ContextQueryCall>);
static_assert(std::is_convertible_v<decltype(&HookEnd), ContextQueryCall>);
static_assert(std::is_convertible_v<decltype(&HookExecuteCommandList), ContextCommandListCall>);
static_assert(std::is_convertible_v<decltype(&HookPsSetShader), PsSetShader>);
static_assert(std::is_convertible_v<decltype(&HookOmSetRenderTargets), OmSetRenderTargets>);
static_assert(std::is_convertible_v<decltype(&HookOmSetRenderTargetsAndUavs), OmSetRenderTargetsAndUavs>);
static_assert(std::is_convertible_v<decltype(&HookDraw), Draw>);
static_assert(std::is_convertible_v<decltype(&HookDrawIndexed), DrawIndexed>);
static_assert(std::is_convertible_v<decltype(&HookDrawInstanced), DrawInstanced>);
static_assert(std::is_convertible_v<decltype(&HookDrawIndexedInstanced), DrawIndexedInstanced>);

}  // namespace fo4hdr
