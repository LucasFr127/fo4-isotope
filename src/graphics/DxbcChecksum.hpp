// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fo4hdr {

// The four words a DXBC container stores after its magic.
using DxbcChecksum = std::array<std::uint32_t, 4>;

// The container's own checksum: MD5 over every byte after the stored checksum,
// with the container's final-block layout in place of MD5's length suffix.
[[nodiscard]] DxbcChecksum ComputeDxbcChecksum(std::span<const std::byte> container) noexcept;

// The checksum of the same container without its reflection, statistics, debug
// and private chunks, which a recompile of one program can add. Empty for a
// malformed container or one without such a chunk.
[[nodiscard]] std::optional<DxbcChecksum> ComputeDxbcProgramChecksum(
    std::span<const std::byte> container) noexcept;

}  // namespace fo4hdr
