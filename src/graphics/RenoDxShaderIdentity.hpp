// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2024 Carlos Lopez
 *
 * Fallout4HDR modification: reduced the pinned RenoDX hash helper to the
 * raw-bytecode CRC-32 operation used by the Fallout 4 shader matcher.
 * Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/utils/hash.hpp
 * See THIRD_PARTY_NOTICES.md.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace fo4hdr {

[[nodiscard]] constexpr std::uint32_t ComputeRenoDxShaderCrc32(
    std::span<const std::byte> bytecode) noexcept
{
    auto crc = std::uint32_t{0xFFFFFFFFu};
    for (const auto value : bytecode)
    {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            const auto reflectedPolynomial = std::uint32_t{0xEDB88320u};
            const auto mask = std::uint32_t{0} - (crc & 1u);
            crc = (crc >> 1u) ^ (reflectedPolynomial & mask);
        }
    }
    return ~crc;
}

}  // namespace fo4hdr
