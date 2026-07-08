// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

namespace fo4hdr {

// Dimensionless scene-linear BT.709/D65 preserves the Academy 0.18 gray anchor.
// Returns contained absolute display-linear BT.2020 nits, without display encoding.
// Negative/nonfinite channels use zero; peaks outside 400-2000 nits fail black.
// A private per-thread cache prepares the peak-specific tables on first use or
// a peak change; this reference entry point is not a render hot-path contract.
[[nodiscard]] LinearRgb MapAces2ToBt2020Nits(
    LinearRgb sceneLinearBt709, double displayPeakNits) noexcept;

}  // namespace fo4hdr
