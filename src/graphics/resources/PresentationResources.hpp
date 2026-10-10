// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace fo4hdr {

inline constexpr std::size_t kMaximumPresentationViews = 8;

struct PresentationResources
{
    IDXGISwapChain* applicationFacadeChain{};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> presentationBackBuffer;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> presentationBackBufferRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> presentationProxy;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> presentationProxyRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> presentationProxySrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> presentationProxyUav;
    std::array<ID3D11RenderTargetView*, kMaximumPresentationViews>
        presentationViews{};
    Microsoft::WRL::ComPtr<ID3D11PixelShader> presentShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> presentVertexShader;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> presentRasterizer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> presentDepth;
    Microsoft::WRL::ComPtr<ID3D11BlendState> presentBlend;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    LinearColorDomain presentationProxyDomain{LinearColorDomain::unknown};
    std::uint64_t colorDomainGeneration{};
    bool presentationRedirectedThisFrame{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool colorSpaceSupported{};
    bool sdrColorSpaceSupported{};
    bool r10Presentation{};
    bool flipModel{};
    std::uint32_t bufferCount{};
    int colorSpaceState{-1};
};
extern PresentationResources g_presentation;

bool PresentationProxyReady() noexcept;
bool PresentationCompositionReady() noexcept;
bool PresentationOutputReady() noexcept;
bool SetColorSpace(bool hdr) noexcept;
bool HdrFlipDescriptorSupported(const DXGI_SWAP_CHAIN_DESC&) noexcept;
bool PresentationResourcesReady() noexcept;
bool CreatePresentationProxy(IDXGISwapChain*, ID3D11Device*) noexcept;
bool RecreatePresentationConstants(ID3D11Device*) noexcept;
bool EnsurePresentationOutputResources(IDXGISwapChain*, ID3D11Device*,
    ID3D11DeviceContext*, bool hdr) noexcept;
bool ApplicationPresentationFacadeActive() noexcept;
HRESULT AcquireApplicationPresentationBuffer(IDXGISwapChain*, ID3D11Texture2D**) noexcept;
enum class PresentationResourceCreation
{
    ready,
    unavailable,
    compositionFailed
};
PresentationResourceCreation CreatePresentationResources(
    IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
void ResetPresentationResources(bool retainColorControl, bool retainPresentationBuffer = false) noexcept;
void RetirePresentationOutput() noexcept;

struct UiTransportResources
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> ui;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uiSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> uiRtv;
};
extern UiTransportResources g_uiTransport;

bool DeveloperUiResourcesReady() noexcept;
bool CreateTextureViews(ID3D11Device*, std::uint32_t width, std::uint32_t height) noexcept;
void ResetUiTransportResources() noexcept;

} // namespace fo4hdr
