// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include <cstddef>
#include <cstdint>
#include <array>

namespace RE { class ImageSpaceManager; }

namespace fo4hdr::game {

struct NativeLutRuntimeFacts
{
    std::uint64_t targetId{};
    std::uintptr_t targetRva{};
    std::array<std::uint8_t, 25> targetBytes{};
    std::uint32_t effectSubobjectOffset{}, textureSrvOffset{};
};

// Requested installation uses the generated exact-runtime map and checked target bytes.
// These checks do not establish in-game pre-use exclusivity or registry lifetime.
[[nodiscard]] bool InitializeNativeLutObserver(bool requested) noexcept;

// The admitted synchronous Gamma Render caller owns manager/texture lifetimes.
// The opaque SRV offset is a reviewed exact-runtime fact, not inferred from RE.
[[nodiscard]] bool ObserveNativeLuts(const RE::ImageSpaceManager* manager,
    std::size_t textureSrvOffset,
    NativeLutObservation& destination) noexcept;

}  // namespace fo4hdr::game
