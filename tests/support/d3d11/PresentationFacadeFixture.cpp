// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "PresentationFacadeFixture.hpp"
#include "GpuHarness.hpp"

namespace fo4hdr::tests::gpu {

HRESULT AcquireFacadeBuffer(IDXGISwapChain* nativeChain, ID3D11Texture2D** buffer) noexcept
{
    if (buffer == nullptr) return E_POINTER;
    *buffer = nullptr;
    if (nativeChain == nullptr) return E_POINTER;
    DXGI_SWAP_CHAIN_DESC description{};
    ComPtr<ID3D11Device> device;
    auto result = nativeChain->GetDesc(&description);
    if (FAILED(result)) return result;
    result = nativeChain->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result)) return result;
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = description.BufferDesc.Width;
    texture.Height = description.BufferDesc.Height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return device->CreateTexture2D(&texture, nullptr, buffer);
}

ComPtr<ID3D11Texture2D> FacadeBuffer(IDXGISwapChain& chain)
{
    ComPtr<ID3D11Texture2D> buffer;
    CheckHresult(chain.GetBuffer(0, IID_PPV_ARGS(&buffer)), "Acquire facade FP16 buffer");
    return buffer;
}

ComPtr<IDXGISwapChain> CreateFacade(IDXGISwapChain& nativeChain)
{
    DXGI_SWAP_CHAIN_DESC application{};
    CheckHresult(nativeChain.GetDesc(&application), "Read native descriptor for facade");
    application.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    application.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    application.BufferCount = 1;
    ComPtr<ID3D11Texture2D> buffer;
    CheckHresult(AcquireFacadeBuffer(&nativeChain, &buffer), "Create genuine facade texture");
    ComPtr<IDXGISwapChain> facade;
    CheckHresult(CreatePresentationSwapChain(
        {&nativeChain, buffer.Get(), application, AcquireFacadeBuffer}, &facade),
        "Create FP16 swapchain facade");
    return facade;
}

} // namespace fo4hdr::tests::gpu
