// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"

#include <array>
#include <cstdint>

namespace fo4hdr::test {

[[nodiscard]] double HalfToDouble(std::uint16_t bits) noexcept;

// Sum 64 signed half coefficients before display containment, independently
// of the shader's grouped eight-read reconstruction.
[[nodiscard]] LinearRgb SampleOperatorLutRaw(
    const OperatorLutData& lut, LinearRgb scene) noexcept;
[[nodiscard]] LinearRgb SampleOperatorLut(
    const OperatorLutData& lut, LinearRgb scene) noexcept;

// Fixed binary32 colors shared by CPU and GPU comparisons.
[[nodiscard]] std::array<LinearRgb, 16> OperatorLutReferenceSamples() noexcept;

}  // namespace fo4hdr::test
