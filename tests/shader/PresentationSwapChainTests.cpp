// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationFacadeFixture.hpp"
#include "hooks/DeviceHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "hooks/SwapChainHooks.hpp"
#include "resources/PresentationResources.hpp"
#include "LiveSettings.hpp"
#include "RendererLifetime.hpp"

#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fo4hdr::tests::gpu {
namespace {

void VerifyFacadeIdentity(Device& device, IDXGISwapChain& facade, IDXGISwapChain& nativeChain)
{
    ComPtr<IUnknown> identity, nativeIdentity;
    CheckHresult(facade.QueryInterface(IID_PPV_ARGS(&identity)), "Read facade COM identity");
    CheckHresult(nativeChain.QueryInterface(IID_PPV_ARGS(&nativeIdentity)), "Read native COM identity");
    FO4HDR_WARP_CHECK(identity.Get() != nativeIdentity.Get());
    const std::array supported{__uuidof(IUnknown), __uuidof(IDXGIObject),
        __uuidof(IDXGIDeviceSubObject), __uuidof(IDXGISwapChain)};
    for (const auto& iid : supported)
    {
        ComPtr<IUnknown> alias, roundtrip;
        CheckHresult(facade.QueryInterface(iid, reinterpret_cast<void**>(alias.GetAddressOf())),
            "Query supported facade interface");
        CheckHresult(alias.As(&roundtrip), "Roundtrip facade canonical IUnknown");
        FO4HDR_WARP_CHECK(roundtrip.Get() == identity.Get());
        ComPtr<IDXGISwapChain> chainAlias;
        CheckHresult(alias.As(&chainAlias), "Roundtrip facade swapchain interface");
        FO4HDR_WARP_CHECK(chainAlias.Get() == &facade);
    }
    const std::array unsupported{__uuidof(IDXGISwapChain1), __uuidof(IDXGISwapChain2),
        __uuidof(IDXGISwapChain3), __uuidof(IDXGISwapChain4)};
    for (const auto& iid : unsupported)
    {
        void* alias = &facade;
        FO4HDR_WARP_CHECK(facade.QueryInterface(iid, &alias) == E_NOINTERFACE);
        FO4HDR_WARP_CHECK(alias == nullptr);
    }
    ComPtr<ID3D11Device> returnedDevice;
    CheckHresult(facade.GetDevice(IID_PPV_ARGS(&returnedDevice)), "Get facade D3D11 device");
    FO4HDR_WARP_CHECK(returnedDevice.Get() == device.device.Get());
    auto buffer = FacadeBuffer(facade);
    ComPtr<ID3D11RenderTargetView> view;
    CheckHresult(device.device->CreateRenderTargetView(buffer.Get(), nullptr, &view),
        "Create RTV from genuine facade texture");
    ComPtr<ID3D11Resource> viewed;
    view->GetResource(&viewed);
    ComPtr<IUnknown> textureIdentity, viewedIdentity;
    CheckHresult(buffer.As(&textureIdentity), "Get facade texture identity");
    CheckHresult(viewed.As(&viewedIdentity), "Get facade RTV resource identity");
    FO4HDR_WARP_CHECK(textureIdentity.Get() == viewedIdentity.Get());
    ComPtr<IDXGISurface> surface;
    CheckHresult(buffer.As(&surface), "Query genuine FP16 DXGI surface");
    ComPtr<ID3D11Texture2D> roundtripTexture;
    CheckHresult(surface.As(&roundtripTexture), "Roundtrip genuine FP16 texture");
    FO4HDR_WARP_CHECK(roundtripTexture.Get() == buffer.Get());
    DXGI_SWAP_CHAIN_DESC description{};
    CheckHresult(facade.GetDesc(&description), "Read application FP16 descriptor");
    D3D11_TEXTURE2D_DESC actual{};
    buffer->GetDesc(&actual);
    D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    FO4HDR_WARP_CHECK(description.BufferDesc.Format == actual.Format);
    FO4HDR_WARP_CHECK(actual.Format == viewDescription.Format);
    FO4HDR_WARP_CHECK(actual.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    FO4HDR_WARP_CHECK(description.BufferDesc.Width == actual.Width);
    FO4HDR_WARP_CHECK(description.BufferDesc.Height == actual.Height);
    FO4HDR_WARP_CHECK(description.SwapEffect == DXGI_SWAP_EFFECT_DISCARD);
    FO4HDR_WARP_CHECK(description.BufferCount == 1);
    void* unavailable = buffer.Get();
    FO4HDR_WARP_CHECK(facade.GetBuffer(1, __uuidof(ID3D11Texture2D), &unavailable) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(unavailable == nullptr);
    CheckDebugMessages(device);
}

void VerifyDescriptorFilteredBackground(Device& device, IDXGISwapChain& facade)
{
    auto buffer = FacadeBuffer(facade);
    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    std::array<std::array<std::uint16_t, 4>, 16 * 8> pixels{};
    // Exact half values isolate copy/filter format closure from transfer-function math.
    pixels.fill({0x0000, 0xc000, 0x4200, 0x3c00});
    pixels.front()[0] = 0x4800;
    device.context->UpdateSubresource(buffer.Get(), 0, nullptr, pixels.data(), 16 * 8, 0);
    ComPtr<ID3D11Texture2D> copy;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr, &copy),
        "Create descriptor-derived CopyResource destination");
    device.context->CopyResource(copy.Get(), buffer.Get());
    description.MipLevels = 2;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> background;
    ComPtr<ID3D11ShaderResourceView> input;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr, &background),
        "Create descriptor-derived FP16 background");
    CheckHresult(device.device->CreateShaderResourceView(background.Get(), nullptr, &input),
        "Create descriptor-derived background SRV");
    device.context->CopySubresourceRegion(background.Get(), 0, 0, 0, 0, copy.Get(), 0, nullptr);
    device.context->GenerateMips(input.Get());
    description.Width /= 2;
    description.Height /= 2;
    description.MipLevels = 1;
    description.MiscFlags = 0;
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr, &readback),
        "Create filtered FP16 background readback");
    device.context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, background.Get(), 1, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read actual GPU low-pass background");
    const auto* filtered = static_cast<const std::uint16_t*>(mapped.pData);
    FO4HDR_WARP_CHECK(filtered[0] == 0x4000);
    FO4HDR_WARP_CHECK(filtered[1] == 0xc000);
    FO4HDR_WARP_CHECK(filtered[2] == 0x4200);
    FO4HDR_WARP_CHECK(filtered[3] == 0x3c00);
    device.context->Unmap(readback.Get(), 0);
    CheckDebugMessages(device);
}

void ExpectFacadeResizeBlocked(Device& device, IDXGISwapChain& facade)
{
    CheckDebugMessages(device);
    FO4HDR_WARP_CHECK(facade.ResizeBuffers(0, 24, 12, DXGI_FORMAT_UNKNOWN, 0) == DXGI_ERROR_INVALID_CALL);
    void* unavailable = &facade;
    FO4HDR_WARP_CHECK(facade.GetBuffer(0, __uuidof(ID3D11Texture2D), &unavailable) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(unavailable == nullptr);
    ComPtr<ID3D11InfoQueue> diagnostics;
    if (SUCCEEDED(device.device.As(&diagnostics))) diagnostics->ClearStoredMessages();
}

void RecoverFacadeResize(Device& device, IDXGISwapChain& facade)
{
    CheckHresult(facade.ResizeBuffers(1, 16, 8, DXGI_FORMAT_UNKNOWN, 0),
        "Recover facade after foreign references retire");
    auto replacement = FacadeBuffer(facade);
    D3D11_TEXTURE2D_DESC description{};
    replacement->GetDesc(&description);
    FO4HDR_WARP_CHECK(description.Width == 16 && description.Height == 8);
    FO4HDR_WARP_CHECK(description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CheckDebugMessages(device);
}

void VerifySurvivingFacadeContent(Device& device, ID3D11Texture2D& buffer)
{
    D3D11_TEXTURE2D_DESC description{};
    buffer.GetDesc(&description);
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr, &readback),
        "Create surviving facade texture readback");
    device.context->CopyResource(readback.Get(), &buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read surviving facade contents after failed resize");
    const auto* pixels = static_cast<const std::uint16_t*>(mapped.pData);
    FO4HDR_WARP_CHECK(pixels[0] == 0xc000 && pixels[1] == 0x4200);
    FO4HDR_WARP_CHECK(pixels[2] == 0x4400 && pixels[3] == 0x3c00);
    device.context->Unmap(readback.Get(), 0);
}

void VerifyFacadeResizeReferences(Device& device, IDXGISwapChain& facade)
{
    auto retained = FacadeBuffer(facade);
    ExpectFacadeResizeBlocked(device, facade);
    D3D11_TEXTURE2D_DESC surviving{};
    retained->GetDesc(&surviving);
    FO4HDR_WARP_CHECK(surviving.Width == 16 && surviving.Height == 8);
    ComPtr<ID3D11RenderTargetView> view;
    CheckHresult(device.device->CreateRenderTargetView(retained.Get(), nullptr, &view),
        "Use surviving foreign texture after failed resize");
    constexpr float color[]{-2.0F, 3.0F, 4.0F, 1.0F};
    device.context->ClearRenderTargetView(view.Get(), color);
    VerifySurvivingFacadeContent(device, *retained.Get());
    view.Reset();
    retained.Reset();
    RecoverFacadeResize(device, facade);

    retained = FacadeBuffer(facade);
    CheckHresult(device.device->CreateRenderTargetView(retained.Get(), nullptr, &view),
        "Create foreign retained facade view");
    retained.Reset();
    ExpectFacadeResizeBlocked(device, facade);
    ComPtr<ID3D11Resource> survivingView;
    view->GetResource(&survivingView);
    FO4HDR_WARP_CHECK(survivingView != nullptr);
    survivingView.Reset();
    view.Reset();
    RecoverFacadeResize(device, facade);

    retained = FacadeBuffer(facade);
    CheckHresult(device.device->CreateRenderTargetView(retained.Get(), nullptr, &view),
        "Create context-owned facade view");
    auto* bound = view.Get();
    device.context->OMSetRenderTargets(1, &bound, nullptr);
    view.Reset();
    retained.Reset();
    ExpectFacadeResizeBlocked(device, facade);
    device.context->OMGetRenderTargets(1, &view, nullptr);
    FO4HDR_WARP_CHECK(view != nullptr);
    device.context->OMSetRenderTargets(0, nullptr, nullptr);
    view.Reset();
    RecoverFacadeResize(device, facade);

    retained = FacadeBuffer(facade);
    CheckHresult(device.device->CreateRenderTargetView(retained.Get(), nullptr, &view),
        "Create recorded-work facade view");
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11CommandList> commands;
    CheckHresult(device.device->CreateDeferredContext(0, &deferred), "Create retained-work context");
    deferred->ClearRenderTargetView(view.Get(), color);
    CheckHresult(deferred->FinishCommandList(FALSE, &commands), "Record facade buffer work");
    view.Reset();
    retained.Reset();
    deferred.Reset();
    ExpectFacadeResizeBlocked(device, facade);
    commands.Reset();
    RecoverFacadeResize(device, facade);
}

void VerifyRejectedFlipPreferences(IDXGISwapChain& nativeChain, ID3D11Texture2D& buffer)
{
    DXGI_SWAP_CHAIN_DESC application{};
    CheckHresult(nativeChain.GetDesc(&application), "Read rejected transport baseline");
    application.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    struct FlipCase { BOOL windowed; UINT count, flags, quality; };
    for (const auto test : {FlipCase{FALSE, 3, 0, 0}, {TRUE, 1, 0, 0}, {TRUE, 0, 0, 0},
             {TRUE, DXGI_MAX_SWAP_CHAIN_BUFFERS + 1, 0, 0}, {TRUE, 3, 0, 1},
             {TRUE, 3, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, 0},
             {TRUE, 3, DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE, 0},
             {TRUE, 3, DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED, 0},
             {TRUE, 3, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, 0}})
    {
        application.Windowed = test.windowed;
        application.BufferCount = test.count;
        application.Flags = test.flags;
        application.SampleDesc.Quality = test.quality;
        ComPtr<IDXGISwapChain> rejected;
        FO4HDR_WARP_CHECK(CreatePresentationSwapChain(
            {&nativeChain, &buffer, application, AcquireFacadeBuffer}, &rejected) == DXGI_ERROR_INVALID_CALL);
        FO4HDR_WARP_CHECK(!rejected);
    }
}

void VerifyFacadeCreationGates(Device& device, IDXGISwapChain& nativeChain)
{
    DXGI_SWAP_CHAIN_DESC application{};
    CheckHresult(nativeChain.GetDesc(&application), "Read unsupported facade descriptor");
    ComPtr<ID3D11Texture2D> buffer;
    CheckHresult(AcquireFacadeBuffer(&nativeChain, &buffer), "Create gated facade buffer");
    VerifyRejectedFlipPreferences(nativeChain, *buffer.Get());
    FO4HDR_WARP_CHECK(AcquireFacadeBuffer(&nativeChain, nullptr) == E_POINTER);
    auto* missing = buffer.Get();
    FO4HDR_WARP_CHECK(AcquireFacadeBuffer(nullptr, &missing) == E_POINTER);
    FO4HDR_WARP_CHECK(missing == nullptr);
    application.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ComPtr<IDXGISwapChain> rejected;
    FO4HDR_WARP_CHECK(CreatePresentationSwapChain(
        {&nativeChain, buffer.Get(), application, AcquireFacadeBuffer}, &rejected) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(!rejected);
    application.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    application.SampleDesc.Count = 4;
    FO4HDR_WARP_CHECK(CreatePresentationSwapChain(
        {&nativeChain, buffer.Get(), application, AcquireFacadeBuffer}, &rejected) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(!rejected);
    application.SampleDesc.Count = 1;
    auto foreign = CreateDevice(false);
    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    ComPtr<ID3D11Texture2D> foreignBuffer;
    CheckHresult(foreign.device->CreateTexture2D(&description, nullptr, &foreignBuffer),
        "Create matching facade texture on another device");
    FO4HDR_WARP_CHECK(CreatePresentationSwapChain(
        {&nativeChain, foreignBuffer.Get(), application, AcquireFacadeBuffer}, &rejected) ==
        DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(!rejected);
    CheckDebugMessages(device);
}

ComPtr<IDXGISwapChain1> CreateTearingPresentationChain(Device& device, HWND window)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    CheckHresult(device.device.As(&dxgi), "Get tearing DXGI device");
    CheckHresult(dxgi->GetAdapter(&adapter), "Get tearing adapter");
    CheckHresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get tearing factory");
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = 16;
    description.Height = 8;
    description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 3;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> chain;
    CheckHresult(factory->CreateSwapChainForHwnd(device.device.Get(), window,
        &description, nullptr, nullptr, &chain), "Create HFPF-like R10 tearing chain");
    return chain;
}

void VerifyFacadeSuccessfulResize(Device& device, IDXGISwapChain& facade,
    IDXGISwapChain& nativeChain)
{
    CheckHresult(facade.ResizeBuffers(3, 24, 12, DXGI_FORMAT_R16G16B16A16_FLOAT, 0),
        "Resize facade dimensions and public buffer count");
    DXGI_SWAP_CHAIN_DESC application{}, native{};
    CheckHresult(facade.GetDesc(&application), "Read resized public facade descriptor");
    CheckHresult(nativeChain.GetDesc(&native), "Read resized native descriptor");
    FO4HDR_WARP_CHECK(application.BufferCount == 3 && native.BufferCount == 3);
    FO4HDR_WARP_CHECK(application.BufferDesc.Width == 24 && application.BufferDesc.Height == 12);
    FO4HDR_WARP_CHECK(native.BufferDesc.Width == 24 && native.BufferDesc.Height == 12);
    FO4HDR_WARP_CHECK(application.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    FO4HDR_WARP_CHECK(native.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    auto buffer = FacadeBuffer(facade);
    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    FO4HDR_WARP_CHECK(description.Width == 24 && description.Height == 12);
    CheckDebugMessages(device);
}

struct NativeFacadeFixture
{
    Device& device;
    RendererDevice savedDevice{g_rendererDevice};
    PresentationResources savedPresentation{g_presentation};
    PluginConfiguration savedConfiguration{g_configuration};
    HdrRendererLifecycle savedLifecycle{g_lifecycle};
    PipelineHeartbeat savedHeartbeat{g_heartbeat};
    bool savedHealthy{g_referenceHealthy.load()};
    PresentationWindow window;
    ComPtr<IDXGISwapChain1> native;
    ComPtr<IDXGISwapChain> facade;
    SwapChainRecord* record{};

    explicit NativeFacadeFixture(Device& value,
        AcquirePresentationBuffer acquire = AcquireApplicationPresentationBuffer,
        bool flipTransport = false) : device(value)
    {
        g_presentation = {};
        g_rendererDevice = {};
        g_rendererDevice.activeDevice = device.device.Get();
        g_rendererDevice.activeContext = device.context.Get();
        CheckHresult(device.context.As(&g_rendererDevice.activeContext1), "Get native facade context1");
        g_configuration.enabled = true;
        g_configuration.productionRequested = true;
        g_lifecycle.Detach();
        FO4HDR_WARP_CHECK(g_lifecycle.BindDeviceAndSwapChain());
        native = flipTransport ? CreateTearingPresentationChain(device, window.handle) :
            CreatePresentationChain(device, window.handle, 16, 8);
        g_presentation.width = 16;
        g_presentation.height = 8;
        FO4HDR_WARP_CHECK(CreatePresentationProxy(native.Get(), device.device.Get()));
        FO4HDR_WARP_CHECK(InstallSwapChainShadow(native.Get(), true, true));
        record = FindSwapChain(native.Get());
        FO4HDR_WARP_CHECK(record != nullptr);
        DXGI_SWAP_CHAIN_DESC application{};
        CheckHresult(native->GetDesc(&application), "Read native integration facade descriptor");
        if (!flipTransport)
        {
            application.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            application.BufferCount = 1;
        }
        CheckHresult(CreatePresentationSwapChain({native.Get(),
            g_presentation.presentationProxy.Get(), application,
            acquire, RetireApplicationPresentationBuffer}, &facade),
            "Create facade over exact production presentation proxy");
        g_presentation.applicationFacadeChain = native.Get();
        record->applicationFp16Facade = true;
    }

    ~NativeFacadeFixture()
    {
        device.context->OMSetRenderTargets(0, nullptr, nullptr);
        facade.Reset();
        g_presentation.applicationFacadeChain = nullptr;
        ResetResources();
        if (record)
        {
            void* expected = record->shadow.data();
            std::atomic_ref<void*> slot(*reinterpret_cast<void**>(native.Get()));
            FO4HDR_WARP_CHECK(slot.compare_exchange_strong(expected, record->originalVtable));
            FO4HDR_WARP_CHECK(record->identity.Retire(native.Get(), record->identity.Generation()));
            record->ownsRendererClaim = false;
            record->applicationFp16Facade = false;
            record->developerHdrDescriptor = false;
        }
        g_rendererDevice = savedDevice;
        g_presentation = savedPresentation;
        g_configuration = savedConfiguration;
        g_lifecycle = savedLifecycle;
        g_heartbeat = savedHeartbeat;
        g_referenceHealthy = savedHealthy;
    }
};

struct FacadePresentProbe
{
    IDXGISwapChain* chain{};
    UINT interval{}, flags{}, calls{};
};
FacadePresentProbe g_facadePresentProbe;

HRESULT STDMETHODCALLTYPE ProbeFacadePresent(IDXGISwapChain* chain, UINT interval, UINT flags)
{
    g_facadePresentProbe = {chain, interval, flags, g_facadePresentProbe.calls + 1};
    return DXGI_STATUS_OCCLUDED;
}

void VerifyFacadePresentPolicy(NativeFacadeFixture& fixture)
{
    const auto next = fixture.record->nextPresent;
    fixture.record->nextPresent = &ProbeFacadePresent;
    struct PresentCase { UINT interval, flags; };
    for (const auto test : {PresentCase{1, 0},
             PresentCase{0, DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_DO_NOT_WAIT}})
    {
        g_facadePresentProbe = {};
        FO4HDR_WARP_CHECK(fixture.facade->Present(test.interval, test.flags) == DXGI_STATUS_OCCLUDED);
        FO4HDR_WARP_CHECK(g_facadePresentProbe.chain == fixture.native.Get());
        FO4HDR_WARP_CHECK(g_facadePresentProbe.interval == test.interval);
        FO4HDR_WARP_CHECK(g_facadePresentProbe.flags == test.flags && g_facadePresentProbe.calls == 1);
    }
    fixture.record->nextPresent = next;
}

void VerifyFacadeFlipTransport(Device& device)
{
    NativeFacadeFixture fixture(device, AcquireApplicationPresentationBuffer, true);
    DXGI_SWAP_CHAIN_DESC application{}, native{};
    CheckHresult(fixture.facade->GetDesc(&application), "Read legacy facade over flip transport");
    CheckHresult(fixture.native->GetDesc(&native), "Read native tearing transport");
    FO4HDR_WARP_CHECK(application.SwapEffect == DXGI_SWAP_EFFECT_DISCARD);
    FO4HDR_WARP_CHECK(application.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    FO4HDR_WARP_CHECK(application.BufferCount == 3 && native.BufferCount == 3);
    FO4HDR_WARP_CHECK(application.Flags == DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    FO4HDR_WARP_CHECK(native.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    FO4HDR_WARP_CHECK(native.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    FO4HDR_WARP_CHECK(native.Flags == (application.Flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));
    void* unavailable = fixture.native.Get();
    FO4HDR_WARP_CHECK(fixture.facade->QueryInterface(__uuidof(IDXGISwapChain4), &unavailable) == E_NOINTERFACE);
    FO4HDR_WARP_CHECK(unavailable == nullptr);
    FO4HDR_WARP_CHECK(fixture.facade->GetBuffer(1, __uuidof(ID3D11Texture2D), &unavailable) ==
        DXGI_ERROR_INVALID_CALL);
    VerifyDescriptorFilteredBackground(device, *fixture.facade.Get());
    VerifyFacadePresentPolicy(fixture);
    {
        auto retained = FacadeBuffer(*fixture.facade.Get());
        ExpectFacadeResizeBlocked(device, *fixture.facade.Get());
    }
    for (const auto flags : {application.Flags, native.Flags})
    {
        CheckHresult(fixture.facade->ResizeBuffers(0, 16, 8, DXGI_FORMAT_UNKNOWN, flags),
            "Preserve native tearing through public or HFPF resize flags");
        CheckHresult(fixture.native->GetDesc(&native), "Read preserved tearing resize");
        FO4HDR_WARP_CHECK(native.BufferCount == 3);
        FO4HDR_WARP_CHECK(native.Flags == (application.Flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));
        FO4HDR_WARP_CHECK(FacadeBuffer(*fixture.facade.Get()).Get() ==
            g_presentation.presentationProxy.Get());
    }
    CheckDebugMessages(device);
}

void VerifyNativeFacadeProxyLifetime(Device& device)
{
    NativeFacadeFixture fixture(device);
    auto exposed = FacadeBuffer(*fixture.facade.Get());
    auto* exact = g_presentation.presentationProxy.Get();
    FO4HDR_WARP_CHECK(exposed.Get() == exact && ApplicationPresentationFacadeActive());
    FO4HDR_WARP_CHECK(fixture.facade->ResizeBuffers(0, 16, 8, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(FacadeBuffer(*fixture.facade.Get()).Get() == exact);
    ResetResources(false, true);
    FO4HDR_WARP_CHECK(g_presentation.presentationProxy.Get() == exact);
    FO4HDR_WARP_CHECK(PresentationProxyReady() && PresentationOutputReady());
    FO4HDR_WARP_CHECK(FacadeBuffer(*fixture.facade.Get()).Get() == exact);
    ComPtr<ID3D11Texture2D> rejected;
    FO4HDR_WARP_CHECK(AcquireApplicationPresentationBuffer(fixture.facade.Get(), &rejected) ==
        DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(!rejected);
    exposed.Reset();
    CheckHresult(fixture.facade->ResizeBuffers(1, 24, 12, DXGI_FORMAT_UNKNOWN, 0),
        "Resize native facade after all owned references retire before flush");
    exposed = FacadeBuffer(*fixture.facade.Get());
    FO4HDR_WARP_CHECK(exposed.Get() == g_presentation.presentationProxy.Get());
    D3D11_TEXTURE2D_DESC description{};
    exposed->GetDesc(&description);
    FO4HDR_WARP_CHECK(description.Width == 24 && description.Height == 12);
    CheckDebugMessages(device);
}

void VerifyNativeFacadeForeignBinding(Device& device)
{
    NativeFacadeFixture fixture(device);
    auto exposed = FacadeBuffer(*fixture.facade.Get());
    ComPtr<ID3D11RenderTargetView> foreign;
    CheckHresult(device.device->CreateRenderTargetView(exposed.Get(), nullptr, &foreign),
        "Create foreign native facade view");
    auto* bound = foreign.Get();
    device.context->OMSetRenderTargets(1, &bound, nullptr);
    foreign.Reset();
    exposed.Reset();
    ExpectFacadeResizeBlocked(device, *fixture.facade.Get());
    device.context->OMGetRenderTargets(1, &foreign, nullptr);
    FO4HDR_WARP_CHECK(foreign.Get() == bound);
    device.context->OMSetRenderTargets(0, nullptr, nullptr);
    foreign.Reset();
    RecoverFacadeResize(device, *fixture.facade.Get());
    FO4HDR_WARP_CHECK(FacadeBuffer(*fixture.facade.Get()).Get() ==
        g_presentation.presentationProxy.Get());
}

HRESULT RejectFacadeBuffer(IDXGISwapChain*, ID3D11Texture2D** buffer) noexcept
{
    *buffer = nullptr;
    return E_FAIL;
}

void VerifyNativeFacadeReacquisitionFailure(Device& device)
{
    NativeFacadeFixture fixture(device, RejectFacadeBuffer);
    FO4HDR_WARP_CHECK(fixture.facade->ResizeBuffers(1, 24, 12, DXGI_FORMAT_UNKNOWN, 0) == E_FAIL);
    FO4HDR_WARP_CHECK(!g_referenceHealthy.load() && !PresentationProxyReady());
    void* unavailable = fixture.facade.Get();
    FO4HDR_WARP_CHECK(fixture.facade->GetBuffer(0, __uuidof(ID3D11Texture2D), &unavailable) ==
        DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(unavailable == nullptr);
    CheckDebugMessages(device);
}

void VerifyFacadeApplicationFormat(Device& device, IDXGISwapChain& facade,
    IDXGISwapChain& nativeChain)
{
    CheckHresult(facade.ResizeBuffers(0, 16, 8, DXGI_FORMAT_R8G8B8A8_UNORM, 0),
        "Resize facade with the application's creation format");
    DXGI_SWAP_CHAIN_DESC application{}, native{};
    CheckHresult(facade.GetDesc(&application), "Read facade after creation-format resize");
    CheckHresult(nativeChain.GetDesc(&native), "Read native after creation-format resize");
    FO4HDR_WARP_CHECK(application.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    FO4HDR_WARP_CHECK(native.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    D3D11_TEXTURE2D_DESC buffer{};
    FacadeBuffer(facade)->GetDesc(&buffer);
    FO4HDR_WARP_CHECK(buffer.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    FO4HDR_WARP_CHECK(buffer.Width == 16 && buffer.Height == 8);
    for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM})
        FO4HDR_WARP_CHECK(facade.ResizeBuffers(0, 16, 8, format, 0) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(FacadeBuffer(facade) != nullptr);

    DXGI_MODE_DESC mode{};
    mode.Width = 16;
    mode.Height = 8;
    const auto forwarded = nativeChain.ResizeTarget(&mode);
    mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    FO4HDR_WARP_CHECK(facade.ResizeTarget(&mode) == forwarded);
    for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM})
    {
        mode.Format = format;
        FO4HDR_WARP_CHECK(facade.ResizeTarget(&mode) == DXGI_ERROR_INVALID_CALL);
    }
    FO4HDR_WARP_CHECK(facade.ResizeTarget(nullptr) == DXGI_ERROR_INVALID_CALL);
    CheckDebugMessages(device);
}

}  // namespace

void VerifyPresentationSwapChain(Device& device)
{
    VerifyFacadeFlipTransport(device);
    VerifyNativeFacadeProxyLifetime(device);
    VerifyNativeFacadeForeignBinding(device);
    VerifyNativeFacadeReacquisitionFailure(device);
    PresentationWindow window;
    auto native = CreatePresentationChain(device, window.handle, 16, 8);
    VerifyFacadeCreationGates(device, *native.Get());
    auto facade = CreateFacade(*native.Get());
    VerifyFacadeIdentity(device, *facade.Get(), *native.Get());
    VerifyDescriptorFilteredBackground(device, *facade.Get());
    VerifyFacadeResizeReferences(device, *facade.Get());
    VerifyFacadeSuccessfulResize(device, *facade.Get(), *native.Get());
    VerifyFacadeApplicationFormat(device, *facade.Get(), *native.Get());
    const auto tested = native->Present(0, DXGI_PRESENT_TEST);
    FO4HDR_WARP_CHECK(facade->Present(0, DXGI_PRESENT_TEST) == tested);
    auto survivor = FacadeBuffer(*facade.Get());
    facade.Reset();
    D3D11_TEXTURE2D_DESC description{};
    survivor->GetDesc(&description);
    FO4HDR_WARP_CHECK(description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    survivor.Reset();
    native.Reset();
    device.context->Flush();
    auto recreatedNative = CreatePresentationChain(device, window.handle, 16, 8);
    auto recreatedFacade = CreateFacade(*recreatedNative.Get());
    VerifyFacadeIdentity(device, *recreatedFacade.Get(), *recreatedNative.Get());
    CheckDebugMessages(device);
}

}  // namespace fo4hdr::tests::gpu
