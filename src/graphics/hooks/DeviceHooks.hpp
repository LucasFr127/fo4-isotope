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

#pragma once

#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/hooks/HookOwnership.hpp"

#include <d3d11.h>

namespace fo4hdr {

ULONG STDMETHODCALLTYPE HookDeviceRelease(ID3D11Device* device) noexcept;

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(
    ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept;

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(
    ID3D11Device* device, const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initialData,
    ID3D11Texture2D** texture) noexcept;

HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description, ID3D11ShaderResourceView** view) noexcept;

HRESULT STDMETHODCALLTYPE HookCreateUav(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* description, ID3D11UnorderedAccessView** view) noexcept;

HRESULT STDMETHODCALLTYPE HookCreateRtv(
    ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* description,
    ID3D11RenderTargetView** view) noexcept;

bool RememberPresentationView(ID3D11Resource*, ID3D11RenderTargetView*) noexcept;
[[nodiscard]] bool InstallContextShadow(ID3D11DeviceContext*) noexcept;
[[nodiscard]] bool InstallSwapChainShadow(IDXGISwapChain*, bool hdrDescriptor,
    bool ownsRendererClaim) noexcept;
void RetireApplicationPresentationBuffer(IDXGISwapChain*) noexcept;

[[nodiscard]] GraphicsObservationFailure InstallGraphicsImport() noexcept;

HRESULT WINAPI HookD3dCreate(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
    UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* description,
    IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** context) noexcept;

HookOwnershipState GraphicsImportHealth() noexcept;

} // namespace fo4hdr
