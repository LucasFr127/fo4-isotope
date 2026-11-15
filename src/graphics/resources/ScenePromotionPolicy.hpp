// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "HdrResourcePolicy.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"

#include <d3d11.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fo4hdr {

// Private-data tag on textures FO4Isotope promoted; only tagged textures get
// their R11G11B10_FLOAT view requests remapped.
inline constexpr GUID kPromotedSceneTexture{0x7c1d4e52, 0x3a9b, 0x4f61,
    {0x9e, 0x2d, 0x58, 0xb3, 0x0c, 0x71, 0xa4, 0xe6}};

inline constexpr UINT kScenePromotionSharingFlags =
    D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
    D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_GDI_COMPATIBLE;

// Project interop gates in front of the RenoDX predicate: FP16 changes the
// byte layout, so CPU-visible, initialized, shared or non-render-target
// textures keep their requested format.
[[nodiscard]] inline bool IsSceneTexturePromotionCandidate(
    const D3D11_TEXTURE2D_DESC& description, bool initialized,
    std::uint32_t backBufferWidth, std::uint32_t backBufferHeight) noexcept
{
    if (initialized || description.Usage != D3D11_USAGE_DEFAULT) return false;
    if (description.CPUAccessFlags != 0) return false;
    if ((description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) return false;
    if ((description.MiscFlags & kScenePromotionSharingFlags) != 0) return false;
    return ShouldPromoteSceneResourceToFp16(
        {
            .format = description.Format == DXGI_FORMAT_R11G11B10_FLOAT ?
                SceneResourceFormat::r11g11b10Float : SceneResourceFormat::other,
            .width = description.Width,
            .height = description.Height
        },
        backBufferWidth,
        backBufferHeight);
}

[[nodiscard]] inline bool ImageContains(
    std::span<const std::byte> image, const void* address) noexcept
{
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(image.data());
    return !image.empty() && value >= begin && value - begin < image.size();
}

// The calling thread's return addresses, innermost first, unwound through the
// game's frames by their .pdata. Returns how many were captured.
inline std::size_t CaptureCallerFrames(std::span<void*> frames) noexcept
{
    return ::RtlCaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
}

// A texture created while one of the initializer's slot-33/34 registrar calls is on
// the stack is a lighting-accumulation target. Zero sites never match.
[[nodiscard]] inline bool KeepsVanillaLightingTarget(
    std::span<void* const> frames, std::span<const std::uintptr_t, 2> sites) noexcept
{
    return std::ranges::any_of(frames, [sites](const void* frame) {
        const auto address = reinterpret_cast<std::uintptr_t>(frame);
        return address != 0 && std::ranges::find(sites, address) != sites.end();
    });
}

// The published registration sites under Vanilla lighting precision, latched per device
// generation; zeros under Full or when the game layer verified none.
[[nodiscard]] std::array<std::uintptr_t, 2> VanillaLightingSites(
    LightingPrecision precision) noexcept;
// Matches the creating thread's frames against latched sites and counts each kept texture.
[[nodiscard]] bool KeepGameLightingFormat(std::span<const std::uintptr_t, 2> sites) noexcept;

}  // namespace fo4hdr
