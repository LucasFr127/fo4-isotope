// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace RE { class ImageSpaceManager; }

namespace fo4hdr::game {

[[nodiscard]] bool NormalizeObservedLutPath(
    std::string_view path, std::array<char, 260>& destination) noexcept;

// Called by the serialized game collector after data-ready. Only that owner
// thread can replace or look up the copied immutable catalog.
[[nodiscard]] bool BuildImageSpaceCatalog(std::uint64_t resourceGeneration) noexcept;
[[nodiscard]] bool ObserveImageSpace(const RE::ImageSpaceManager* manager,
    std::uint64_t resourceGeneration, ImageSpaceSnapshot& destination) noexcept;

}  // namespace fo4hdr::game
