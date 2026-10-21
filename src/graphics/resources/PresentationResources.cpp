// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "PresentationResources.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../DisplayDiscovery.hpp"
#include "../passes/PresentationPass.hpp"
#include "../SharedUiFrame.hpp"
#include <FO4Isotope/ShippingShaders.generated.hpp>
#include <algorithm>
#include <utility>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

UiTransportResources g_uiTransport;

struct RenderTextureDestination
{
    ComPtr<ID3D11Texture2D>& texture;
    ComPtr<ID3D11ShaderResourceView>& srv;
    ComPtr<ID3D11RenderTargetView>& rtv;
};

[[nodiscard]] bool CreateRenderTextureViews(
    ID3D11Device* device, const D3D11_TEXTURE2D_DESC& description,
    const RenderTextureDestination& output) noexcept
{
    return SUCCEEDED(device->CreateTexture2D(&description, nullptr, &output.texture)) &&
        SUCCEEDED(device->CreateShaderResourceView(output.texture.Get(), nullptr, &output.srv)) &&
        SUCCEEDED(device->CreateRenderTargetView(output.texture.Get(), nullptr, &output.rtv));
}

[[nodiscard]] bool CreateTextureViews(
    ID3D11Device* device, std::uint32_t width, std::uint32_t height) noexcept
{
    D3D11_TEXTURE2D_DESC fp16{};
    fp16.Width = width;
    fp16.Height = height;
    fp16.MipLevels = 1;
    fp16.ArraySize = 1;
    fp16.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16.SampleDesc.Count = 1;
    fp16.Usage = D3D11_USAGE_DEFAULT;
    fp16.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                     D3D11_BIND_RENDER_TARGET;
    return CreateRenderTextureViews(device, fp16,
        {g_uiTransport.ui, g_uiTransport.uiSrv, g_uiTransport.uiRtv});
}


[[nodiscard]] bool DeveloperUiResourcesReady() noexcept
{
    return g_uiTransport.ui && g_uiTransport.uiSrv && g_uiTransport.uiRtv;
}

void ResetUiTransportResources() noexcept
{
    g_uiTransport.uiRtv.Reset();
    g_uiTransport.uiSrv.Reset();
    g_uiTransport.ui.Reset();
}

PresentationResources g_presentation;

[[nodiscard]] bool ApplicationPresentationFacadeActive() noexcept
{
    return g_presentation.applicationFacadeChain != nullptr;
}

HRESULT AcquireApplicationPresentationBuffer(
    IDXGISwapChain* swapChain, ID3D11Texture2D** buffer) noexcept
{
    if (buffer == nullptr) return E_POINTER;
    *buffer = nullptr;
    if (swapChain == nullptr || swapChain != g_presentation.applicationFacadeChain)
        return DXGI_ERROR_INVALID_CALL;
    if (!PresentationProxyReady())
        return DXGI_ERROR_INVALID_CALL;
    return g_presentation.presentationProxy.CopyTo(buffer);
}

[[nodiscard]] bool PresentationProxyReady() noexcept
{
    return g_presentation.presentationBackBuffer && g_presentation.presentationProxy &&
        g_presentation.presentationProxyRtv && g_presentation.presentationProxySrv;
}

[[nodiscard]] bool PresentationCompositionReady() noexcept
{
    return g_presentation.presentShader && g_presentation.presentVertexShader &&
        g_presentation.presentRasterizer && g_presentation.presentDepth &&
        g_presentation.presentBlend && g_presentation.constants;
}

[[nodiscard]] bool PresentationOutputReady() noexcept
{
    return g_presentation.presentationBackBuffer && g_presentation.presentationBackBufferRtv;
}

[[nodiscard]] bool MatchesPresentationBuffer(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    return description.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        description.Width == g_presentation.width &&
        description.Height == g_presentation.height && description.SampleDesc.Count == 1;
}

[[nodiscard]] bool SupportsFp16TypedUav(ID3D11Device* device) noexcept
{
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{DXGI_FORMAT_R16G16B16A16_FLOAT, 0};
    constexpr UINT typed = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    return SUCCEEDED(device->CheckFeatureSupport(
        D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) &&
        (support.OutFormatSupport2 & typed) == typed;
}

struct PresentationProxyCreation
{
    D3D11_TEXTURE2D_DESC description{};
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> outputRtv;
    ComPtr<ID3D11Texture2D> proxy;
    ComPtr<ID3D11UnorderedAccessView> proxyUav;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;

    [[nodiscard]] bool AcquireOutput(IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
    {
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
        backBuffer->GetDesc(&description);
        if (!MatchesPresentationBuffer(description)) return false;
        return SUCCEEDED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &outputRtv));
    }

    [[nodiscard]] bool CreateProxy(ID3D11Device* device) noexcept
    {
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.CPUAccessFlags = 0;
        description.MiscFlags = 0;
        if (SupportsFp16TypedUav(device)) description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &proxy))) {
            // Optional typed-UAV support must never break ordinary HDR creation.
            proxy.Reset();
            description.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(device->CreateTexture2D(&description, nullptr, &proxy))) return false;
        }
        if ((description.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0)
            (void)device->CreateUnorderedAccessView(proxy.Get(), nullptr, &proxyUav);
        return SUCCEEDED(device->CreateRenderTargetView(proxy.Get(), nullptr, &rtv)) &&
            SUCCEEDED(device->CreateShaderResourceView(proxy.Get(), nullptr, &srv));
    }

    void Publish() noexcept
    {
        RetireSharedUiPublicImage();
        g_presentation.presentationBackBuffer = std::move(backBuffer);
        g_presentation.presentationBackBufferRtv = std::move(outputRtv);
        g_presentation.presentationProxy = std::move(proxy);
        g_presentation.presentationProxyRtv = std::move(rtv);
        g_presentation.presentationProxySrv = std::move(srv);
        g_presentation.presentationProxyUav = std::move(proxyUav);
        g_evidence.presentationProxyCreateCount.fetch_add(1, std::memory_order_relaxed);
    }
};

bool PresentationSourceReady() noexcept
{
    return g_presentation.presentationProxy && g_presentation.presentationProxySrv;
}

bool EnsurePresentationTarget(IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
{
    if (g_presentation.presentationBackBuffer)
        return g_presentation.presentationBackBufferRtv ||
            SUCCEEDED(device->CreateRenderTargetView(g_presentation.presentationBackBuffer.Get(),
                nullptr, &g_presentation.presentationBackBufferRtv));
    PresentationProxyCreation creation;
    if (!creation.AcquireOutput(swapChain, device)) return false;
    g_presentation.presentationBackBuffer = std::move(creation.backBuffer);
    g_presentation.presentationBackBufferRtv = std::move(creation.outputRtv);
    return true;
}

[[nodiscard]] bool CreatePresentationProxy(
    IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
{
    if (swapChain == nullptr || device == nullptr) return false;
    if (PresentationSourceReady()) return EnsurePresentationTarget(swapChain, device);
    PresentationProxyCreation creation;
    if (!creation.AcquireOutput(swapChain, device)) return false;
    if (!creation.CreateProxy(device)) return false;
    creation.Publish();
    return true;
}
[[nodiscard]] bool PresentationResourcesReady() noexcept
{
    const std::array required{
        PresentationOutputReady(),
        DeveloperUiResourcesReady(),
        PresentationCompositionReady()
    };
    return std::ranges::all_of(required, [](bool ready) { return ready; });
}

[[nodiscard]] bool HdrFlipDescriptorSupported(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    return description.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        description.BufferCount >= 2 &&
        (description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
            description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
}

[[nodiscard]] bool ReadHdrResourceDescriptor(IDXGISwapChain* swapChain,
    ID3D11Device* device, ID3D11DeviceContext* context, DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    const std::array<const void*, 3> required{swapChain, device, context};
    if (std::ranges::find(required, nullptr) != required.end()) return false;
    if (FAILED(swapChain->GetDesc(&description))) return false;
    return HdrFlipDescriptorSupported(description);
}

[[nodiscard]] bool SupportsPresentationTarget(ID3D11Device* device) noexcept
{
    UINT formatSupport{};
    return SUCCEEDED(device->CheckFormatSupport(
        DXGI_FORMAT_R10G10B10A2_UNORM, &formatSupport)) &&
        (formatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;
}

[[nodiscard]] bool SupportsPresentationColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept
{
    UINT support{};
    return SUCCEEDED(g_presentation.swapChain3->CheckColorSpaceSupport(colorSpace, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
}

[[nodiscard]] bool ValidateOutputColorSupport(
    IDXGISwapChain* swapChain, ID3D11Device* device, HWND window) noexcept
{
    if (!SupportsPresentationTarget(device)) return false;
    if (!g_presentation.swapChain3 &&
        FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&g_presentation.swapChain3))))
        return false;
    if (!RefreshDisplayCapabilities(window)) return false;
    if (CurrentDisplayCapabilities().description.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        return false;
    g_presentation.sdrColorSpaceSupported =
        SupportsPresentationColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    g_presentation.colorSpaceSupported =
        SupportsPresentationColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) &&
        g_presentation.sdrColorSpaceSupported;
    return g_presentation.colorSpaceSupported;
}

[[nodiscard]] bool ValidateSdrOutputColorSupport(
    IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
{
    if (!SupportsPresentationTarget(device)) return false;
    if (!g_presentation.swapChain3 &&
        FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&g_presentation.swapChain3))))
        return false;
    g_presentation.sdrColorSpaceSupported =
        SupportsPresentationColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    return g_presentation.sdrColorSpaceSupported;
}

[[nodiscard]] bool InitializeResourceDimensions(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    g_presentation.r10Presentation = true;
    g_presentation.flipModel = true;
    g_presentation.bufferCount = description.BufferCount;
    g_presentation.width = description.BufferDesc.Width;
    g_presentation.height = description.BufferDesc.Height;
    if (g_presentation.width == 0 || g_presentation.height == 0) {
        RECT client{};
        if (!GetClientRect(description.OutputWindow, &client)) return false;
        g_presentation.width = static_cast<std::uint32_t>(
            std::max<LONG>(0, client.right - client.left));
        g_presentation.height = static_cast<std::uint32_t>(
            std::max<LONG>(0, client.bottom - client.top));
    }
    return true;
}

[[nodiscard]] bool CreatePresentationShaders(ID3D11Device* device) noexcept
{
    return SUCCEEDED(device->CreatePixelShader(generated::kPresentPixelsPs.data(),
            generated::kPresentPixelsPs.size(), nullptr, &g_presentation.presentShader)) &&
        SUCCEEDED(device->CreateVertexShader(generated::kPresentFullscreenVs.data(),
            generated::kPresentFullscreenVs.size(), nullptr, &g_presentation.presentVertexShader));
}

[[nodiscard]] bool CreatePresentationPipelineStates(ID3D11Device* device) noexcept
{
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateRasterizerState(&rasterizer, &g_presentation.presentRasterizer)) &&
        SUCCEEDED(device->CreateDepthStencilState(&depth, &g_presentation.presentDepth)) &&
        SUCCEEDED(device->CreateBlendState(&blend, &g_presentation.presentBlend));
}

[[nodiscard]] bool RecreatePresentationConstants(ID3D11Device* device) noexcept
{
    if (device == nullptr) return false;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(CompositionConstants);
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> candidate;
    if (FAILED(device->CreateBuffer(&description, nullptr, &candidate))) return false;
    g_presentation.constants = std::move(candidate);
    return true;
}

[[nodiscard]] bool CreatePresentationComposition(ID3D11Device* device) noexcept
{
    if (g_presentation.width == 0 || g_presentation.height == 0) return false;
    if (!CreatePresentationShaders(device) || !CreatePresentationPipelineStates(device)) return false;
    return RecreatePresentationConstants(device);
}

bool PresentationColorSupported(bool hdr) noexcept
{
    return hdr ? g_presentation.colorSpaceSupported : g_presentation.sdrColorSpaceSupported;
}

bool PresentationOutputResourcesReady(bool hdr) noexcept
{
    const std::array required{PresentationColorSupported(hdr), PresentationSourceReady(),
        PresentationOutputReady(), PresentationCompositionReady()};
    return std::ranges::all_of(required, [](bool ready) { return ready; });
}

bool PreparePresentationOutputColor(IDXGISwapChain* swapChain,
    ID3D11Device* device, ID3D11DeviceContext* context, bool hdr) noexcept
{
    DXGI_SWAP_CHAIN_DESC description{};
    if (!ReadHdrResourceDescriptor(swapChain, device, context, description)) return false;
    const bool colorSupport = hdr ?
        ValidateOutputColorSupport(swapChain, device, description.OutputWindow) :
        ValidateSdrOutputColorSupport(swapChain, device);
    return colorSupport && InitializeResourceDimensions(description);
}

[[nodiscard]] bool EnsurePresentationOutputResources(IDXGISwapChain* swapChain,
    ID3D11Device* device, ID3D11DeviceContext* context, bool hdr) noexcept
{
    if (PresentationOutputResourcesReady(hdr)) return true;
    if (!PresentationColorSupported(hdr) &&
        !PreparePresentationOutputColor(swapChain, device, context, hdr)) return false;
    if (!CreatePresentationProxy(swapChain, device)) return false;
    if (!PresentationCompositionReady() && !CreatePresentationComposition(device)) return false;
    // A reset also retires the optional settings layer; output stays usable without it.
    if (!DeveloperUiResourcesReady())
        (void)CreateTextureViews(device, g_presentation.width, g_presentation.height);
    return true;
}

[[nodiscard]] PresentationResourceCreation CreatePresentationResources(
    IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    DXGI_SWAP_CHAIN_DESC description{};
    if (!ReadHdrResourceDescriptor(swapChain, device, context, description)) {
        RecordFailure(GraphicsObservationFailure::descriptorUnavailable);
        return PresentationResourceCreation::unavailable;
    }
    if (!ValidateOutputColorSupport(swapChain, device, description.OutputWindow)) {
        RecordFailure(GraphicsObservationFailure::colorSpaceUnsupported);
        return PresentationResourceCreation::unavailable;
    }
    if (!InitializeResourceDimensions(description)) return PresentationResourceCreation::unavailable;
    if (!CreatePresentationComposition(device)) {
        RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
        return PresentationResourceCreation::compositionFailed;
    }
    // Flattened public output remains usable when optional layer capture fails.
    (void)CreateTextureViews(device, g_presentation.width, g_presentation.height);
    return PresentationResourceCreation::ready;
}


[[nodiscard]] bool SetColorSpace(bool hdr) noexcept
{
    if (!g_presentation.swapChain3 || !PresentationColorSupported(hdr))
        return false;
    const auto desired = hdr ? 1 : 0;
    if (g_presentation.colorSpaceState == desired) return true;
    const auto space = hdr ?
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 :
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (FAILED(g_presentation.swapChain3->SetColorSpace1(space)))
    {
        if (hdr) RecordFailure(GraphicsObservationFailure::colorSpaceRejected);
        return false;
    }
    g_presentation.colorSpaceState = desired;
    g_evidence.hdrColorSpaceActive.store(hdr, std::memory_order_release);
    return true;
}


void RetirePresentationOutput() noexcept
{
    // Armor closes before this phase; its borrowed proxy UAV retires before output.
    g_presentation.presentationProxyUav.Reset();
    g_presentation.presentationBackBufferRtv.Reset();
}

void ResetPresentationResources(bool retainColorControl, bool retainPresentationBuffer) noexcept
{
    if (!retainPresentationBuffer)
    {
        RetireSharedUiPublicImage();
        g_presentation.presentationProxySrv.Reset();
        g_presentation.presentationProxyRtv.Reset();
        g_presentation.presentationProxy.Reset();
        g_presentation.presentationBackBuffer.Reset();
        g_presentation.r10Presentation = false;
        g_presentation.flipModel = false;
        g_presentation.bufferCount = 0;
        g_presentation.width = 0;
        g_presentation.height = 0;
    }
    g_presentation.constants.Reset();
    g_presentation.presentShader.Reset();
    g_presentation.presentVertexShader.Reset();
    g_presentation.presentRasterizer.Reset();
    g_presentation.presentDepth.Reset();
    g_presentation.presentBlend.Reset();
    // A failed resize leaves this same chain alive. Its control interface holds
    // no buffer view and is needed to recover SDR if the transition was rejected.
    if (!retainColorControl)
    {
        g_presentation.swapChain3.Reset();
        g_presentation.colorSpaceSupported = false;
        g_presentation.sdrColorSpaceSupported = false;
        g_presentation.colorSpaceState = -1;
    }
    g_presentation.colorDomainGeneration = 0;
    g_evidence.colorDomainGeneration.store(0, std::memory_order_release);
    g_evidence.hdrColorSpaceActive.store(
        retainColorControl && g_presentation.colorSpaceState == 1,
        std::memory_order_release);
    g_evidence.productionActive.store(false, std::memory_order_release);
}

} // namespace fo4hdr
