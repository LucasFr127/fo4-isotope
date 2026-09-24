// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace fo4hdr::test {

[[nodiscard]] double HalfToDouble(std::uint16_t bits) noexcept;

// Independently sum all 64 coefficient-lattice contributions from the actual
// half-precision texture rather than sharing the shader's grouped fetches.
[[nodiscard]] LinearRgb SampleOperatorLut(
    const OperatorLutData& lut, LinearRgb scene) noexcept;

[[nodiscard]] std::vector<LinearRgb> BuildNearBlackOperatorCorpus();

// Deterministic held-out colors are independent of LUT knots and shapers.
[[nodiscard]] std::vector<LinearRgb> BuildOperatorLutHoldoutCorpus();

// Shared CPU/GPU interpolation coverage uses identical binary32 inputs.
[[nodiscard]] std::vector<LinearRgb> BuildOperatorLutCorpus(double peak);

[[nodiscard]] std::array<LinearRgb, 16> OperatorLutReferenceSamples() noexcept;

} // namespace fo4hdr::test
