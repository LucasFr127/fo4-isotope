// SPDX-License-Identifier: GPL-3.0-or-later

#include "ContextHooks.hpp"
#include "../HdrRenderer.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>

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

}  // namespace fo4hdr
