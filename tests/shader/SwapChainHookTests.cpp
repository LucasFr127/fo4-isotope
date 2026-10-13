// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "hooks/HookRecords.hpp"
#include "hooks/SwapChainHooks.hpp"
#include "LiveSettings.hpp"
#include "RendererLifetime.hpp"
#include "GraphicsDiagnostics.hpp"

namespace fo4hdr::tests::gpu {

struct PresentProbe
{
    IDXGISwapChain* chain{};
    UINT interval{}, flags{}, calls{};
};
PresentProbe g_presentProbe;

HRESULT STDMETHODCALLTYPE ProbePresent(IDXGISwapChain* chain, UINT interval, UINT flags)
{
    g_presentProbe = {chain, interval, flags, g_presentProbe.calls + 1};
    return DXGI_STATUS_OCCLUDED;
}

void VerifyTestPresent(IDXGISwapChain* chain, SwapChainRecord& record)
{
    const auto next = record.nextPresent;
    const auto frame = g_frame;
    const auto count = g_evidence.presentCount.load();
    record.nextPresent = &ProbePresent;
    g_presentProbe = {};
    constexpr auto flags = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT;
    const auto result = HookPresent(chain, 3, flags);
    record.nextPresent = next;
    FO4HDR_WARP_CHECK(result == DXGI_STATUS_OCCLUDED);
    FO4HDR_WARP_CHECK(g_presentProbe.chain == chain && g_presentProbe.calls == 1);
    FO4HDR_WARP_CHECK(g_presentProbe.interval == 3 && g_presentProbe.flags == flags);
    FO4HDR_WARP_CHECK(g_frame == frame && g_evidence.presentCount.load() == count);
}

struct PresentationResizeHookScope
{
    IDXGISwapChain* chain;
    SwapChainRecord& record{g_swapChains.front()};
    PluginConfiguration configuration{g_configuration};
    HdrRendererLifecycle lifecycle{g_lifecycle};
    bool referenceHealthy{g_referenceHealthy.load()};

    explicit PresentationResizeHookScope(IDXGISwapChain* swapChain) : chain(swapChain)
    {
        FO4HDR_WARP_CHECK(record.identity.Publish(chain, 1));
        auto** vtable = *reinterpret_cast<void***>(chain);
        record.nextResizeBuffers = reinterpret_cast<ResizeBuffers>(vtable[kSwapChainResizeBuffers]);
        record.ownsRendererClaim = true;
        record.developerHdrDescriptor = true;
        g_configuration.enabled = true;
        g_configuration.productionRequested = true;
        FO4HDR_WARP_CHECK(g_lifecycle.BindDeviceAndSwapChain());
    }

    ~PresentationResizeHookScope()
    {
        ResetResources();
        (void)record.identity.Retire(chain, 1);
        record.nextResizeBuffers = nullptr;
        record.ownsRendererClaim = false;
        record.developerHdrDescriptor = false;
        g_configuration = configuration;
        g_lifecycle = lifecycle;
        g_referenceHealthy = referenceHealthy;
    }
};

void VerifyPresentationResize(Device& device)
{
    constexpr UINT width = 37, height = 19;
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, width, height);
    CheckHresult(chain->ResizeBuffers(3, width, height, DXGI_FORMAT_UNKNOWN, 0),
        "Create three-buffer resize baseline");
    ComPtr<IDXGISwapChain> base;
    CheckHresult(chain.As(&base), "Get base resize interface");
    PresentationResizeHookScope scope(base.Get());
    VerifyTestPresent(base.Get(), scope.record);
    FO4HDR_WARP_CHECK(g_rendererDevice.activeDevice == nullptr);
    struct ResizeCase { UINT requested, expected; };
    for (const auto test : {ResizeCase{0, 3}, {1, 2}, {2, 2}, {3, 3}, {8, 8}, {0, 8}})
    {
        CheckHresult(HookResizeBuffers(base.Get(), test.requested, width, height,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0), "Resize through production HDR hook");
        DXGI_SWAP_CHAIN_DESC actual{};
        CheckHresult(base->GetDesc(&actual), "Read production resize descriptor");
        FO4HDR_WARP_CHECK(actual.BufferCount == test.expected);
        FO4HDR_WARP_CHECK(actual.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
        FO4HDR_WARP_CHECK(actual.BufferDesc.Width == width && actual.BufferDesc.Height == height);
        FO4HDR_WARP_CHECK(actual.Flags == 0);
        FO4HDR_WARP_CHECK(!g_referenceHealthy.load());
    }
    auto retained = GetPresentationBuffer(device, *chain.Get());
    ComPtr<ID3D11InfoQueue> diagnostics;
    (void)device.device.As(&diagnostics);
    CheckDebugMessages(device);
    FO4HDR_WARP_CHECK(FAILED(HookResizeBuffers(base.Get(), 0, width, height,
        DXGI_FORMAT_UNKNOWN, 0)));
    if (diagnostics) diagnostics->ClearStoredMessages();
    retained = {};
    CheckHresult(HookResizeBuffers(base.Get(), 0, width, height, DXGI_FORMAT_UNKNOWN, 0),
        "Recover resize after releasing buffer references");
    DXGI_SWAP_CHAIN_DESC recovered{};
    CheckHresult(base->GetDesc(&recovered), "Read recovered resize descriptor");
    FO4HDR_WARP_CHECK(recovered.BufferCount == 8);
    CheckDebugMessages(device);
}

}  // namespace fo4hdr::tests::gpu
