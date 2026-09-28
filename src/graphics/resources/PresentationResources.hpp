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
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> presentationBackBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> presentationBackBufferUav;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> presentationProxy;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> presentationProxyRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> presentationProxySrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> presentationProxyUav;
    std::array<ID3D11RenderTargetView*, kMaximumPresentationViews>
        presentationViews{};
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> presentShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    LinearColorDomain presentationProxyDomain{LinearColorDomain::unknown};
    std::uint64_t colorDomainGeneration{};
    bool presentationRedirectedThisFrame{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool colorSpaceSupported{};
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
enum class PresentationResourceCreation
{
    ready,
    unavailable,
    compositionFailed
};
PresentationResourceCreation CreatePresentationResources(
    IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
void ResetPresentationResources(bool retainColorControl) noexcept;
void RetirePresentationOutput() noexcept;

struct UiTransportResources
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> ui;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uiSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> uiRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gameUiBackdrop;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gameUiBackdropSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> gameUiBackdropRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gameUi;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gameUiSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> gameUiRtv;
    Microsoft::WRL::ComPtr<ID3D11BlendState> gameUiBlend;
    bool gameUiBackdropCapturedThisFrame{};
    bool gameUiCapturedThisFrame{};
};
extern UiTransportResources g_uiTransport;

bool DeveloperUiResourcesReady() noexcept;
bool GameUiBackdropResourcesReady() noexcept;
bool GameUiResourcesReady() noexcept;
bool UiCaptureResourcesReady() noexcept;
bool CreateTextureViews(ID3D11Device*, std::uint32_t width, std::uint32_t height) noexcept;
void ResetUiTransportResources() noexcept;
void ResetUiTransportCaptureState() noexcept;

} // namespace fo4hdr
