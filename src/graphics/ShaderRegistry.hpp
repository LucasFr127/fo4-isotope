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

#include "PipboyScreenObservation.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include <d3d11.h>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

inline constexpr GUID kPipboyScreenShaderRole{0x626617e9, 0x64b7, 0x4725,
    {0xaa, 0x26, 0x94, 0xf4, 0x62, 0x95, 0x13, 0x72}};
inline constexpr GUID kNativeSceneCompositeShaderRole{0x42fd9fa0, 0x72b4, 0x4693,
    {0x86, 0x17, 0x3d, 0xb2, 0xb6, 0x5e, 0x90, 0x0a}};
inline constexpr GUID kFrameTraceShaderCrc{0xa9074e87, 0xf4b9, 0x4398,
    {0x9a, 0xb4, 0xa0, 0x42, 0x7a, 0xcb, 0x7f, 0x56}};
[[nodiscard]] const RenoDxShaderDescriptor* FindShaderDescriptor(RenoDxShaderPass pass) noexcept;
[[nodiscard]] const RenoDxShaderDescriptor* FindShader(ID3D11PixelShader* shader) noexcept;
[[nodiscard]] bool IsLateGameUiShader(ID3D11PixelShader* shader) noexcept;
[[nodiscard]] bool IsLateGameUiBackdropShader(ID3D11PixelShader* shader) noexcept;
[[nodiscard]] bool IsNativeSceneCompositeShader(ID3D11PixelShader* shader) noexcept;
void ResetShaderRegistry() noexcept;

struct CreatedShaderIdentity
{
    const RenoDxShaderDescriptor* descriptor{};
    bool lateGameUiComposite{};
    bool lateGameUiBackdrop{};
    bool nativeSceneComposite{};
    PipboyScreenShader pipboyRole{};
    std::uint32_t diagnosticCrc{};

    void Observe(const void* bytecode, std::size_t length, bool developerMode);
    void Register(HRESULT result, ID3D11PixelShader** shader) const;

private:
    static bool PipboyHashMismatch(PipboyScreenShader role, const void* bytecode, std::size_t length);
    static bool CreatedSuccessfully(HRESULT result, ID3D11PixelShader** shader);
    void RecordKnownShader(std::uint32_t crc32) const;
    void AttachPipboyRole(HRESULT result, ID3D11PixelShader** shader) const;
    void AttachNativeSceneRole(ID3D11PixelShader* shader) const;
};

}  // namespace fo4hdr
