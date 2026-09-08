// SPDX-License-Identifier: GPL-3.0-or-later

#include "SwapChainHooks.hpp"
#include "HookRecords.hpp"

#include <algorithm>
#include <type_traits>
#include <memory>

namespace fo4hdr {
namespace {

bool DeviceLost(HRESULT result) noexcept
{
    constexpr std::array failures{DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
        DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR};
    return std::find(failures.begin(), failures.end(), result) != failures.end();
}

bool SameChainLayout(const DXGI_SWAP_CHAIN_DESC& left, const DXGI_SWAP_CHAIN_DESC& right) noexcept
{
    return left.BufferDesc.Width == right.BufferDesc.Width &&
        left.BufferDesc.Height == right.BufferDesc.Height &&
        left.BufferDesc.Format == right.BufferDesc.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality;
}

bool SameChainWindow(const DXGI_SWAP_CHAIN_DESC& left, const DXGI_SWAP_CHAIN_DESC& right) noexcept
{
    return left.OutputWindow == right.OutputWindow && left.Windowed == right.Windowed &&
        left.BufferCount == right.BufferCount && left.SwapEffect == right.SwapEffect &&
        left.BufferUsage == right.BufferUsage && left.Flags == right.Flags;
}

bool CurrentChainDescription(SwapChainHookRecord& record) noexcept
{
    DXGI_SWAP_CHAIN_DESC current{};
    if (FAILED(record.owned->GetDesc(&current))) return false;
    if (!IsWindow(current.OutputWindow)) return false;
    return SameChainLayout(current, record.description) && SameChainWindow(current, record.description);
}

class ChainCallScope
{
public:
    explicit ChainCallScope(SwapChainHookRecord& record) noexcept : record_(record)
    {
        if (!record.admitted.load(std::memory_order_acquire)) return;
        if (record.busy.test_and_set(std::memory_order_acquire)) { RevokeNativeRouting(); return; }
        entered_ = true;
        const auto* context = g_contextHooks.load(std::memory_order_acquire);
        if (context == nullptr) { RevokeNativeRouting(); return; }
        if (context->thread != GetCurrentThreadId()) { RevokeNativeRouting(); return; }
        if (!NativeRoutingOwned()) { RevokeNativeRouting(); return; }
        valid_ = true;
    }
    ~ChainCallScope() noexcept
    {
        if (entered_) record_.busy.clear(std::memory_order_release);
    }
    ChainCallScope(const ChainCallScope&) = delete;
    ChainCallScope& operator=(const ChainCallScope&) = delete;
    [[nodiscard]] bool Valid() const noexcept { return valid_; }

private:
    SwapChainHookRecord& record_;
    bool entered_{};
    bool valid_{};
};

class ChainResourcePreparation
{
public:
    explicit ChainResourcePreparation(ContextHookRecord& context) noexcept : context_(context)
    {
        context_.internalWork = true;
    }
    ~ChainResourcePreparation() noexcept { context_.internalWork = false; }
    ChainResourcePreparation(const ChainResourcePreparation&) = delete;
    ChainResourcePreparation& operator=(const ChainResourcePreparation&) = delete;

private:
    ContextHookRecord& context_;
};

template <class Call, class... Arguments>
HRESULT CallNative(Call next, Arguments... arguments) noexcept
{
    try { return next(arguments...); }
    catch (...) { return E_FAIL; }
}

bool ComposeNativeOutput(ContextHookRecord& context) noexcept
{
    const bool gameplay = NativeGameplayReady(context);
    const auto packet = context.uiCapture.CompletedLayers();
    const bool layers = context.presentation.PrepareNativeLayers(packet, PresentationScope::qualifiedExclusive);
    static_cast<void>(context.presentation.PrepareNativeSdrHandoff(
        context.presentationProxy.CompletedFrame(), PresentationScope::qualifiedExclusive));
    const bool hdr = gameplay && context.renderer.CurrentSceneOperatorProduced() && layers;
    return context.presentation.FinishNativeFrame(context.frameRedirected, hdr, PresentationScope::qualifiedExclusive);
}

bool PresentQuerySafe(ContextHookRecord& context) noexcept
{
    if (context.queries.CanDispatch()) return true;
    static_cast<void>(context.renderer.EndFrame());
    RevokeNativeRouting();
    return false;
}

bool PreparePresent(SwapChainHookRecord& chain, UINT flags) noexcept
{
    if (!chain.resourcesReady.load(std::memory_order_acquire)) return false;
    if (!CurrentChainDescription(chain)) { RevokeNativeRouting(); return false; }
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    if ((flags & DXGI_PRESENT_TEST) != 0) return true;
    if (!PresentQuerySafe(*admitted)) return false;
    const ChainResourcePreparation preparation{*admitted};
    const bool completed = admitted->hdrPrepared ? ComposeNativeOutput(*admitted)
        : CloseNativePresentFrame(admitted->renderer, flags);
    admitted->presentationProxy.InvalidateFrame();
    admitted->uiCapture.CloseFrame();
    admitted->frameRedirected = false;
    return completed;
}

void CompletePresent(SwapChainHookRecord& chain, UINT flags, HRESULT result) noexcept
{
    if (!chain.resourcesReady.load(std::memory_order_acquire)) return;
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return;
    if (!admitted->queries.CanDispatch()) return;
    if ((flags & DXGI_PRESENT_TEST) != 0 || FAILED(result)) return;
    const ChainResourcePreparation preparation{*admitted};
    const bool opened = admitted->hdrPrepared
        ? admitted->presentation.BeginNativeFrame(PresentationScope::qualifiedExclusive)
        : OpenNativePresentFrame(admitted->renderer, flags, result);
    if (!opened) RevokeNativeRouting();
    else admitted->uiCapture.AdvanceFrame(admitted->renderer.FrameGeneration());
}

bool RetireChainResources(ContextHookRecord& context) noexcept
{
    const ChainResourcePreparation preparation{context};
    if (context.hdrPrepared && !context.presentation.RestoreNativeSdrSpace()) return false;
    if (!context.presentation.Retire()) return false;
    if (!context.presentationProxy.Retire()) return false;
    if (!context.uiCapture.Retire()) return false;
    return context.renderer.RetireForResize();
}

bool PrepareResize(SwapChainHookRecord& chain) noexcept
{
    chain.resourcesReady.store(false, std::memory_order_release);
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    admitted->presentationProxy.RevokePublication();
    if (!admitted->queries.CanDispatch()) return false;
    return RetireChainResources(*admitted);
}

bool ChainResourcesCurrent(const ContextHookRecord& context) noexcept
{
    return context.admitted.load(std::memory_order_acquire) && NativeRoutingOwned() &&
        context.queries.CanDispatch();
}

bool RebuildHdrOutput(ContextHookRecord& context, ID3D11Device* device,
    IDXGISwapChain* chain, ScenePassConfiguration configuration) noexcept
{
    if (FAILED(context.renderer.AdoptPresentationScene(context.presentationProxy, context.presentationScene))) return false;
    std::destroy_at(&context.presentation);
    std::construct_at(&context.presentation);
    const PresentationConfiguration output{configuration.width, configuration.height,
        static_cast<float>(context.defaultOperator.displayPeakNits), &context.renderer, configuration.whites};
    if (!context.presentation.Initialize(device, context.owned, output)) return false;
    return SUCCEEDED(context.presentation.PrepareNativeChain(chain, true));
}

bool RebuildChainResources(ContextHookRecord& context, ID3D11Device* device,
    IDXGISwapChain* chain, ScenePassConfiguration configuration) noexcept
{
    const ChainResourcePreparation preparation{context};
    if (!context.renderer.ReinitializeBorrowed(device, context.owned, configuration)) return false;
    const bool ui = context.uiCapture.Initialize(device, context.owned, configuration);
    const bool proxy = context.presentationProxy.Prepare(device, context.owned, chain);
    if (context.hdrPrepared)
    {
        if (!ui || !proxy) return false;
        if (!RebuildHdrOutput(context, device, chain, configuration)) return false;
    }
    if (!ChainResourcesCurrent(context))
    {
        context.presentationProxy.RevokePublication();
        return false;
    }
    return true;
}

bool ReadResizeDescription(const SwapChainHookRecord& chain, DXGI_SWAP_CHAIN_DESC& current) noexcept
{
    if (FAILED(chain.owned->GetDesc(&current))) return false;
    return current.OutputWindow == chain.description.OutputWindow && IsWindow(current.OutputWindow);
}

bool ReinitializeAfterResize(SwapChainHookRecord& chain) noexcept
{
    DXGI_SWAP_CHAIN_DESC current{};
    if (!ReadResizeDescription(chain, current)) return false;
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const auto* device = g_deviceHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    if (!admitted->queries.CanDispatch()) return false;
    const ScenePassConfiguration configuration{current.BufferDesc.Width, current.BufferDesc.Height, {},
        admitted->hdrPrepared ? &admitted->defaultOperator : nullptr};
    if (!RebuildChainResources(*admitted, device->owned, chain.owned, configuration)) return false;
    if (!chain.admitted.load(std::memory_order_acquire)) return false;
    chain.description = current;
    chain.resourcesReady.store(true, std::memory_order_release);
    return true;
}

struct NativeResizeRequest
{
    UINT count;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT flags;
};

void CompleteResize(SwapChainHookRecord& record, bool retired, HRESULT result) noexcept
{
    if (DeviceLost(result)) { RevokeNativeRouting(); return; }
    if (!retired || FAILED(result)) return;
    if (!ReinitializeAfterResize(record)) RevokeNativeRouting();
}

void PreserveHdrResize(NativeResizeRequest& request, bool retired) noexcept
{
    const auto* context = g_contextHooks.load(std::memory_order_acquire);
    if (!retired || context == nullptr) return;
    if (!context->hdrPrepared) return;
    if (request.count != 0) request.count = 2;
    if (request.format != DXGI_FORMAT_UNKNOWN) request.format = DXGI_FORMAT_R10G10B10A2_UNORM;
}

HRESULT ResizeOwnedChain(SwapChainHookRecord& record, NativeResizeRequest request) noexcept
{
    const ChainCallScope call{record};
    const bool retired = call.Valid() && PrepareResize(record);
    PreserveHdrResize(request, retired);
    const auto result = CallNative(record.nextResize, record.owned, request.count,
        request.width, request.height, request.format, request.flags);
    CompleteResize(record, retired, result);
    return result;
}

}  // namespace

bool NativeChainOwned() noexcept
{
    auto* record = g_swapChainHooks.load(std::memory_order_acquire);
    if (record == nullptr) return false;
    const auto* context = g_contextHooks.load(std::memory_order_acquire);
    if (context == nullptr || !context->retainedReferences.load(std::memory_order_acquire)) return false;
    if (*reinterpret_cast<void***>(record->owned) != record->table) return false;
    return std::all_of(record->owners.begin(), record->owners.end(),
        [](auto& slot) { return slot.Health() == HookOwnershipState::owned; });
}

bool CloseNativePresentFrame(ScenePassRenderer& renderer, UINT flags) noexcept
{
    if ((flags & DXGI_PRESENT_TEST) != 0) return true;
    return renderer.EndFrame();
}

bool OpenNativePresentFrame(ScenePassRenderer& renderer, UINT flags, HRESULT result) noexcept
{
    if ((flags & DXGI_PRESENT_TEST) != 0) return true;
    if (FAILED(result)) return true;
    return renderer.BeginFrame();
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT interval, UINT flags) noexcept
{
    auto* record = g_swapChainHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    if (chain != record->owned) return CallNative(record->nextPresent, chain, interval, flags);
    const ChainCallScope call{*record};
    const bool prepared = call.Valid() && PreparePresent(*record, flags);
    // The context scope is gone before native callbacks can issue query/draw calls.
    const auto result = CallNative(record->nextPresent, chain, interval, flags);
    if (DeviceLost(result)) RevokeNativeRouting();
    else if (prepared) CompletePresent(*record, flags, result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* chain, UINT count, UINT width,
    UINT height, DXGI_FORMAT format, UINT flags) noexcept
{
    auto* record = g_swapChainHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    if (chain != record->owned)
        return CallNative(record->nextResize, chain, count, width, height, format, flags);
    return ResizeOwnedChain(*record, {count, width, height, format, flags});
}

static_assert(std::is_convertible_v<decltype(&HookPresent), SwapChainPresent>);
static_assert(std::is_convertible_v<decltype(&HookResizeBuffers), SwapChainResize>);

}  // namespace fo4hdr
