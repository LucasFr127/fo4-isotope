// SPDX-License-Identifier: GPL-3.0-or-later

#include "SwapChainHooks.hpp"
#include "HookRecords.hpp"

#include <algorithm>
#include <type_traits>

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

bool PreparePresent(SwapChainHookRecord& chain, UINT flags) noexcept
{
    if (!chain.resourcesReady.load(std::memory_order_acquire)) return false;
    if (!CurrentChainDescription(chain)) { RevokeNativeRouting(); return false; }
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    if (!admitted->queries.CanDispatch())
    {
        if ((flags & DXGI_PRESENT_TEST) == 0) static_cast<void>(admitted->renderer.EndFrame());
        return false;
    }
    return CloseNativePresentFrame(admitted->renderer, flags);
}

void CompletePresent(SwapChainHookRecord& chain, UINT flags, HRESULT result) noexcept
{
    if (!chain.resourcesReady.load(std::memory_order_acquire)) return;
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return;
    if (!admitted->queries.CanDispatch()) return;
    if (!OpenNativePresentFrame(admitted->renderer, flags, result)) RevokeNativeRouting();
}

bool PrepareResize(SwapChainHookRecord& chain) noexcept
{
    chain.resourcesReady.store(false, std::memory_order_release);
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    return admitted->renderer.RetireForResize();
}

bool RebuildChainResources(ContextHookRecord& context, ID3D11Device* device,
    ScenePassConfiguration configuration) noexcept
{
    const ChainResourcePreparation preparation{context};
    if (!context.renderer.ReinitializeBorrowed(device, context.owned, configuration)) return false;
    if (!context.admitted.load(std::memory_order_acquire)) return false;
    if (!NativeRoutingOwned()) return false;
    return context.queries.CanDispatch();
}

bool ReinitializeAfterResize(SwapChainHookRecord& chain) noexcept
{
    DXGI_SWAP_CHAIN_DESC current{};
    if (FAILED(chain.owned->GetDesc(&current))) return false;
    if (current.OutputWindow != chain.description.OutputWindow) return false;
    if (!IsWindow(current.OutputWindow)) return false;
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const auto* device = g_deviceHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    if (!admitted->queries.CanDispatch()) return false;
    const ScenePassConfiguration configuration{current.BufferDesc.Width, current.BufferDesc.Height, {}};
    if (!RebuildChainResources(*admitted, device->owned, configuration)) return false;
    if (!chain.admitted.load(std::memory_order_acquire)) return false;
    chain.description = current;
    chain.resourcesReady.store(true, std::memory_order_release);
    return true;
}

}  // namespace

bool NativeChainOwned() noexcept
{
    auto* record = g_swapChainHooks.load(std::memory_order_acquire);
    if (record == nullptr) return false;
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
    const ChainCallScope call{*record};
    const bool retired = call.Valid() && PrepareResize(*record);
    // No owned backbuffer aliases exist; borrowed retirement preserves foreign bindings.
    const auto result = CallNative(record->nextResize, chain, count, width, height, format, flags);
    if (DeviceLost(result)) RevokeNativeRouting();
    else if (retired && SUCCEEDED(result)) static_cast<void>(ReinitializeAfterResize(*record));
    return result;
}

static_assert(std::is_convertible_v<decltype(&HookPresent), SwapChainPresent>);
static_assert(std::is_convertible_v<decltype(&HookResizeBuffers), SwapChainResize>);

}  // namespace fo4hdr
