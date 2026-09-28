// SPDX-License-Identifier: MIT
// Copyright (C) 2023 Carlos Lopez
// Copyright (C) 2024 Carlos Lopez
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/addon.cpp
// Upstream context: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/utils/resource.hpp
// Whole-file simplified adaptation for FO4Isotope. Modification: reduce the
// generic RenoDX resource-upgrade description to the trusted Fallout 4
// R11G11B10_FLOAT/backbuffer-aspect predicate and its exact 0.0001 tolerance.
// See THIRD_PARTY_NOTICES.md.
#pragma once

#include <cmath>
#include <cstdint>

namespace fo4hdr {

enum class SceneResourceFormat : std::uint8_t
{
    other,
    r11g11b10Float,
    r16g16b16a16Float
};

struct SceneResourceDescription
{
    SceneResourceFormat format{SceneResourceFormat::other};
    std::uint32_t width{};
    std::uint32_t height{};
};

inline constexpr float kSceneResourceAspectTolerance = 0.0001f;

// RenoDX Fallout 4's trusted predicate: only an R11G11B10_FLOAT resource
// whose aspect ratio matches the backbuffer is eligible for FP16 promotion.
// Resolution equality is intentionally not required.
[[nodiscard]] inline bool ShouldPromoteSceneResourceToFp16(
    const SceneResourceDescription& resource,
    std::uint32_t backBufferWidth,
    std::uint32_t backBufferHeight) noexcept
{
    if (resource.format != SceneResourceFormat::r11g11b10Float) return false;
    if (resource.width == 0 || resource.height == 0) return false;
    if (backBufferWidth == 0 || backBufferHeight == 0) return false;

    const auto resourceAspect =
        static_cast<float>(resource.width) /
        static_cast<float>(resource.height);
    const auto backBufferAspect =
        static_cast<float>(backBufferWidth) /
        static_cast<float>(backBufferHeight);
    return std::abs(resourceAspect - backBufferAspect) <=
           kSceneResourceAspectTolerance;
}

}  // namespace fo4hdr
