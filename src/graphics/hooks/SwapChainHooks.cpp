// SPDX-License-Identifier: GPL-3.0-or-later

#include "SwapChainHooks.hpp"
#include "HookRecords.hpp"

#include <algorithm>
#include <type_traits>
#include <memory>
#include <tuple>

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
    return std::tie(left.BufferDesc.Width, left.BufferDesc.Height, left.BufferDesc.Format,
               left.SampleDesc.Count, left.SampleDesc.Quality) ==
        std::tie(right.BufferDesc.Width, right.BufferDesc.Height, right.BufferDesc.Format,
            right.SampleDesc.Count, right.SampleDesc.Quality);
}

bool SameChainWindow(const DXGI_SWAP_CHAIN_DESC& left, const DXGI_SWAP_CHAIN_DESC& right) noexcept
{
    return std::tie(left.OutputWindow, left.Windowed, left.BufferCount, left.SwapEffect,
               left.BufferUsage, left.Flags) ==
        std::tie(right.OutputWindow, right.Windowed, right.BufferCount, right.SwapEffect,
            right.BufferUsage, right.Flags);
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
    ChainResourcePreparation() noexcept
    {
        ActiveNativeOwner().internalWork = true;
    }
    ~ChainResourcePreparation() noexcept { ActiveNativeOwner().internalWork = false; }
    ChainResourcePreparation(const ChainResourcePreparation&) = delete;
    ChainResourcePreparation& operator=(const ChainResourcePreparation&) = delete;

};

template <class Call, class... Arguments>
HRESULT CallNative(Call next, Arguments... arguments) noexcept
{
    try { return next(arguments...); }
    catch (...) { return E_FAIL; }
}

bool NativeSceneHdrReady(const ScenePassRenderer& renderer) noexcept
{
    if (!ReadNativeDiagnosticProjection().referenceRequested) return renderer.CurrentSceneOperatorProduced();
    if (renderer.Views(renderer.CurrentScene()).texture == nullptr) return false;
    const auto domain = renderer.CurrentSceneDomain();
    constexpr std::array domains{SceneColorDomain::bt709Scene, SceneColorDomain::bt2020UiRelative};
    return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

bool ComposeNativeOutput(ContextHookRecord& context) noexcept
{
    const bool gameplay = NativeGameplayReady(context);
    const auto packet = ActiveNativeOwner().uiCapture.CompletedLayers();
    const bool layers = ActiveNativeOwner().presentation.PrepareNativeLayers(packet, PresentationScope::qualifiedExclusive);
    static_cast<void>(ActiveNativeOwner().presentation.PrepareNativeSdrHandoff(
        ActiveNativeOwner().presentationProxy.CompletedFrame(), PresentationScope::qualifiedExclusive));
    const bool hdr = gameplay && NativeSceneHdrReady(ActiveNativeOwner().renderer) && layers;
    const bool completed = ActiveNativeOwner().presentation.FinishNativeFrame(
        ActiveNativeOwner().frameRedirected, hdr, PresentationScope::qualifiedExclusive);
    RecordNativeFrameTrace(hdr && completed);
    return completed;
}

bool PresentQuerySafe(ContextHookRecord& context) noexcept
{
    if (context.queries.CanDispatch()) return true;
    static_cast<void>(ActiveNativeOwner().renderer.EndFrame());
    RevokeNativeRouting();
    return false;
}

void CommitNativeSettings(bool completed) noexcept
{
    if (!completed || !ActiveNativeOwner().hdrPrepared) return;
    static_cast<void>(ActiveNativeOwner().settingsHost.CommitClosedFrame(PresentationScope::qualifiedExclusive));
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
    const ChainResourcePreparation preparation;
    const bool completed = ActiveNativeOwner().hdrPrepared ? ComposeNativeOutput(*admitted)
        : CloseNativePresentFrame(ActiveNativeOwner().renderer, flags);
    if (!ActiveNativeOwner().hdrPrepared) RecordNativeFrameTrace(false);
    CommitNativeSettings(completed);
    ActiveNativeOwner().presentationProxy.InvalidateFrame();
    ActiveNativeOwner().uiCapture.CloseFrame();
    ActiveNativeOwner().frameRedirected = false;
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
    const ChainResourcePreparation preparation;
    const bool opened = ActiveNativeOwner().hdrPrepared
        ? ActiveNativeOwner().presentation.BeginNativeFrame(PresentationScope::qualifiedExclusive)
        : OpenNativePresentFrame(ActiveNativeOwner().renderer, flags, result);
    if (!opened) RevokeNativeRouting();
    else ActiveNativeOwner().uiCapture.AdvanceFrame(ActiveNativeOwner().renderer.FrameGeneration());
}

bool RetireChainResources() noexcept
{
    const ChainResourcePreparation preparation;
    if (ActiveNativeOwner().hdrPrepared && !ActiveNativeOwner().presentation.RestoreNativeSdrSpace()) return false;
    if (!ActiveNativeOwner().presentation.Retire()) return false;
    if (!ActiveNativeOwner().presentationProxy.Retire()) return false;
    if (!ActiveNativeOwner().uiCapture.Retire()) return false;
    return ActiveNativeOwner().renderer.RetireForResize();
}

bool PrepareResize(SwapChainHookRecord& chain) noexcept
{
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    const NativeRoutingScope scope{context->owned};
    auto* admitted = scope.Record();
    if (admitted == nullptr) return false;
    if (!admitted->queries.CanDispatch()) return false;
    // A held gesture or nested HWND callback refuses before any owner retirement.
    if (!ActiveNativeOwner().settingsHost.SuspendSurfaceForResize()) return false;
    chain.resourcesReady.store(false, std::memory_order_release);
    ActiveNativeOwner().presentationProxy.RevokePublication();
    return RetireChainResources();
}

bool ChainResourcesCurrent(const ContextHookRecord& context) noexcept
{
    return context.admitted.load(std::memory_order_acquire) && NativeRoutingOwned() &&
        context.queries.CanDispatch();
}

bool RebuildHdrOutput(ContextHookRecord& context, ID3D11Device* device,
    IDXGISwapChain* chain, ScenePassConfiguration configuration) noexcept
{
    if (FAILED(ActiveNativeOwner().renderer.AdoptPresentationScene(ActiveNativeOwner().presentationProxy, ActiveNativeOwner().presentationScene))) return false;
    std::destroy_at(&ActiveNativeOwner().presentation);
    std::construct_at(&ActiveNativeOwner().presentation);
    PresentationConfiguration output;
    if (!ActiveNativeOwner().settingsHost.ReadActiveOutputConfiguration(configuration.width, configuration.height,
            ActiveNativeOwner().renderer, output)) return false;
    output.measurePerformance = ReadNativePerformanceProjection().requested;
    if (!ActiveNativeOwner().presentation.Initialize(device, context.owned, output)) return false;
    if (FAILED(ActiveNativeOwner().presentation.PrepareNativeChain(chain, true))) return false;
    return ActiveNativeOwner().settingsHost.ResumeAfterResize(configuration.width, configuration.height,
        PresentationScope::qualifiedExclusive);
}

bool ReadResizeWhites(ScenePassConfiguration& configuration) noexcept
{
    if (!ActiveNativeOwner().hdrPrepared) return true;
    PresentationConfiguration active;
    if (!ActiveNativeOwner().settingsHost.ReadActiveOutputConfiguration(configuration.width, configuration.height,
            ActiveNativeOwner().renderer, active)) return false;
    configuration.whites = active.whites;
    return true;
}

bool RebuildChainResources(ContextHookRecord& context, ID3D11Device* device,
    IDXGISwapChain* chain, ScenePassConfiguration configuration) noexcept
{
    const ChainResourcePreparation preparation;
    if (!ReadResizeWhites(configuration)) return false;
    if (!ActiveNativeOwner().renderer.ReinitializeBorrowed(device, context.owned, configuration)) return false;
    const bool ui = ActiveNativeOwner().uiCapture.Initialize(device, context.owned, configuration, ReadNativeDiagnosticProjection().referenceRequested);
    const bool proxy = ActiveNativeOwner().presentationProxy.Prepare(device, context.owned, chain);
    if (ActiveNativeOwner().hdrPrepared)
    {
        if (!ui || !proxy) return false;
        if (!RebuildHdrOutput(context, device, chain, configuration)) return false;
    }
    if (!ChainResourcesCurrent(context))
    {
        ActiveNativeOwner().presentationProxy.RevokePublication();
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
    // Reapply the retained active table after reconstruction; never build here or
    // resurrect the startup operator while a newer accepted tuple exists.
    const ScenePassConfiguration configuration{current.BufferDesc.Width, current.BufferDesc.Height};
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
    if (!ActiveNativeOwner().hdrPrepared) return;
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
    if (record == nullptr || !record->identity.Matches(record->owned)) return false;
    const auto* context = g_contextHooks.load(std::memory_order_acquire);
    if (context == nullptr || !context->retainedReferences.load(std::memory_order_acquire)) return false;
    if (*reinterpret_cast<void***>(record->owned) != record->table) return false;
    if (record->table[2] != reinterpret_cast<void*>(&HookSwapChainRelease)) return false;
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
