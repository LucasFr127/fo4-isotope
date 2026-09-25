// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
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
#include <Fallout4HDR/ShippingShaders.generated.hpp>
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
    if (!CreateRenderTextureViews(device, fp16,
            {g_uiTransport.ui, g_uiTransport.uiSrv, g_uiTransport.uiRtv}))
        return false;

    if (!CreateRenderTextureViews(device, fp16,
            {g_uiTransport.gameUiBackdrop, g_uiTransport.gameUiBackdropSrv,
                g_uiTransport.gameUiBackdropRtv}))
        return false;

    D3D11_TEXTURE2D_DESC gameUi = fp16;
    gameUi.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!CreateRenderTextureViews(device, gameUi,
            {g_uiTransport.gameUi, g_uiTransport.gameUiSrv, g_uiTransport.gameUiRtv}))
        return false;

    D3D11_BLEND_DESC uiBlend{};
    auto& target = uiBlend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&uiBlend, &g_uiTransport.gameUiBlend));
}


[[nodiscard]] bool DeveloperUiResourcesReady() noexcept
{
    return g_uiTransport.ui && g_uiTransport.uiSrv && g_uiTransport.uiRtv;
}

[[nodiscard]] bool GameUiBackdropResourcesReady() noexcept
{
    return g_uiTransport.gameUiBackdrop && g_uiTransport.gameUiBackdropSrv &&
        g_uiTransport.gameUiBackdropRtv;
}

[[nodiscard]] bool GameUiResourcesReady() noexcept
{
    return g_uiTransport.gameUi && g_uiTransport.gameUiSrv && g_uiTransport.gameUiRtv &&
        g_uiTransport.gameUiBlend;
}

[[nodiscard]] bool UiCaptureResourcesReady() noexcept
{
    return g_uiTransport.gameUiBackdropRtv && g_uiTransport.gameUiRtv && g_uiTransport.gameUiBlend;
}

void ResetUiTransportResources() noexcept
{
    g_uiTransport.gameUiBlend.Reset();
    g_uiTransport.gameUiRtv.Reset();
    g_uiTransport.gameUiSrv.Reset();
    g_uiTransport.gameUi.Reset();
    g_uiTransport.gameUiBackdropRtv.Reset();
    g_uiTransport.gameUiBackdropSrv.Reset();
    g_uiTransport.gameUiBackdrop.Reset();
    g_uiTransport.uiRtv.Reset();
    g_uiTransport.uiSrv.Reset();
    g_uiTransport.ui.Reset();
}

void ResetUiTransportCaptureState() noexcept
{
    g_uiTransport.gameUiBackdropCapturedThisFrame = false;
    g_uiTransport.gameUiCapturedThisFrame = false;
}

PresentationResources g_presentation;

template <std::size_t Size>
[[nodiscard]] HRESULT CreateComputeShader(
    ID3D11Device* device, const std::array<std::byte, Size>& bytecode,
    ID3D11ComputeShader** shader) noexcept
{
    return device->CreateComputeShader(
        bytecode.data(), bytecode.size(), nullptr, shader);
}

[[nodiscard]] bool PresentationProxyReady() noexcept
{
    return g_presentation.presentationBackBuffer && g_presentation.presentationProxy &&
        g_presentation.presentationProxyRtv && g_presentation.presentationProxySrv;
}

[[nodiscard]] bool PresentationCompositionReady() noexcept
{
    return g_presentation.presentShader && g_presentation.constants;
}

[[nodiscard]] bool PresentationOutputReady() noexcept
{
    return g_presentation.presentationBackBuffer && g_presentation.presentationBackBufferUav;
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
    ComPtr<ID3D11UnorderedAccessView> outputUav;
    ComPtr<ID3D11Texture2D> proxy;
    ComPtr<ID3D11UnorderedAccessView> proxyUav;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;

    [[nodiscard]] bool AcquireOutput(IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
    {
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
        backBuffer->GetDesc(&description);
        if (!MatchesPresentationBuffer(description)) return false;
        return SUCCEEDED(device->CreateUnorderedAccessView(backBuffer.Get(), nullptr, &outputUav));
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
        g_presentation.presentationViews.fill(nullptr);
        g_presentation.presentationBackBuffer = std::move(backBuffer);
        g_presentation.presentationBackBufferUav = std::move(outputUav);
        g_presentation.presentationProxy = std::move(proxy);
        g_presentation.presentationProxyRtv = std::move(rtv);
        g_presentation.presentationProxySrv = std::move(srv);
        g_presentation.presentationProxyUav = std::move(proxyUav);
        g_evidence.presentationProxyCreateCount.fetch_add(1, std::memory_order_relaxed);
    }
};

[[nodiscard]] bool CreatePresentationProxy(
    IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
{
    if (PresentationProxyReady() && g_presentation.presentationBackBufferUav) return true;
    if (swapChain == nullptr || device == nullptr) return false;
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
        DeveloperUiResourcesReady(), GameUiBackdropResourcesReady(), GameUiResourcesReady(),
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

[[nodiscard]] bool SupportsPresentationUav(ID3D11Device* device) noexcept
{
    UINT formatSupport{};
    return SUCCEEDED(device->CheckFormatSupport(
        DXGI_FORMAT_R10G10B10A2_UNORM, &formatSupport)) &&
        (formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
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
    if (!SupportsPresentationUav(device)) return false;
    if (!g_presentation.swapChain3 &&
        FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&g_presentation.swapChain3))))
        return false;
    if (!RefreshDisplayCapabilities(window)) return false;
    if (CurrentDisplayCapabilities().description.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        return false;
    return SupportsPresentationColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) &&
        SupportsPresentationColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
}

[[nodiscard]] bool InitializeResourceDimensions(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    g_presentation.colorSpaceSupported = true;
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

[[nodiscard]] bool CreatePresentationComposition(ID3D11Device* device) noexcept
{
    if (g_presentation.width == 0 || g_presentation.height == 0) return false;
    if (!CreateTextureViews(device, g_presentation.width, g_presentation.height)) return false;
    if (FAILED(CreateComputeShader(device, generated::kPresentPixelsCs, &g_presentation.presentShader)))
        return false;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(CompositionConstants);
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&description, nullptr, &g_presentation.constants));
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
    return PresentationResourceCreation::ready;
}


[[nodiscard]] bool SetColorSpace(bool hdr) noexcept
{
    if (!g_presentation.swapChain3 || !g_presentation.colorSpaceSupported)
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
    g_presentation.presentationBackBufferUav.Reset();
}

void ResetPresentationResources(bool retainColorControl) noexcept
{
    g_presentation.presentationProxySrv.Reset();
    g_presentation.presentationProxyRtv.Reset();
    g_presentation.presentationProxy.Reset();
    g_presentation.presentationBackBuffer.Reset();
    g_presentation.presentationViews.fill(nullptr);
    ResetUiTransportCaptureState();
    g_presentation.constants.Reset();
    g_presentation.presentShader.Reset();
    // A failed resize leaves this same chain alive. Its control interface holds
    // no buffer view and is needed to recover SDR if the transition was rejected.
    if (!retainColorControl)
    {
        g_presentation.swapChain3.Reset();
        g_presentation.colorSpaceSupported = false;
        g_presentation.colorSpaceState = -1;
    }
    g_presentation.r10Presentation = false;
    g_presentation.flipModel = false;
    g_presentation.bufferCount = 0;
    g_presentation.width = 0;
    g_presentation.height = 0;
    g_presentation.colorDomainGeneration = 0;
    g_evidence.colorDomainGeneration.store(0, std::memory_order_release);
    g_evidence.hdrColorSpaceActive.store(
        retainColorControl && g_presentation.colorSpaceState == 1,
        std::memory_order_release);
    g_evidence.productionActive.store(false, std::memory_order_release);
}

} // namespace fo4hdr
