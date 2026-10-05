// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationFacadeFixture.hpp"

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

}  // namespace

void VerifyPresentationSwapChain(Device& device)
{
    PresentationWindow window;
    auto native = CreatePresentationChain(device, window.handle, 16, 8);
    VerifyFacadeCreationGates(device, *native.Get());
    auto facade = CreateFacade(*native.Get());
    VerifyFacadeIdentity(device, *facade.Get(), *native.Get());
    VerifyDescriptorFilteredBackground(device, *facade.Get());
    VerifyFacadeResizeReferences(device, *facade.Get());
    VerifyFacadeSuccessfulResize(device, *facade.Get(), *native.Get());
    FO4HDR_WARP_CHECK(facade->ResizeBuffers(0, 16, 8, DXGI_FORMAT_R8G8B8A8_UNORM, 0) == DXGI_ERROR_INVALID_CALL);
    FO4HDR_WARP_CHECK(FacadeBuffer(*facade.Get()) != nullptr);
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
