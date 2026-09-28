// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"

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

} // namespace fo4hdr::test
