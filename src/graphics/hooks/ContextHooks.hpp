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

#include "../DrawObservation.hpp"

#include <d3d11.h>

namespace fo4hdr {

ULONG STDMETHODCALLTYPE HookContextRelease(
    ID3D11DeviceContext* context) noexcept;

void STDMETHODCALLTYPE HookBegin(
    ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept;

void STDMETHODCALLTYPE HookEnd(
    ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept;

void STDMETHODCALLTYPE HookExecuteCommandList(
    ID3D11DeviceContext* context, ID3D11CommandList* commands,
    BOOL restoreState) noexcept;

void STDMETHODCALLTYPE HookOmSetRenderTargetsAndUavs(
    ID3D11DeviceContext* context, UINT rtvCount,
    ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* depth, UINT uavStart, UINT uavCount,
    ID3D11UnorderedAccessView* const* uavs,
    const UINT* initialCounts) noexcept;

void STDMETHODCALLTYPE HookDraw(
    ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) noexcept;

void STDMETHODCALLTYPE HookDrawIndexed(
    ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) noexcept;

void STDMETHODCALLTYPE HookDrawIndexedInstanced(
    ID3D11DeviceContext* context, UINT indices, UINT instances,
    UINT startIndex, INT baseVertex, UINT startInstance) noexcept;

void STDMETHODCALLTYPE HookDrawInstanced(
    ID3D11DeviceContext* context, UINT vertices, UINT instances,
    UINT startVertex, UINT startInstance) noexcept;

void STDMETHODCALLTYPE HookPsSetShader(
    ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classes, UINT classCount) noexcept;

void STDMETHODCALLTYPE HookOmSetRenderTargets(
    ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* depth) noexcept;

bool ValidateExpectedShaderBinding(ID3D11DeviceContext*, DrawObservation&) noexcept;

} // namespace fo4hdr
