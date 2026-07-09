// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

namespace fo4hdr {

// Scene-linear BT.709/D65 uses the fixed 203-nit scene reference white.
// Returns contained absolute display-linear BT.2020 nits, without display encoding.
// Fixed AgX response: EV 0, contrast 2.7, HDR purity 0.5 and shoulder power 2.
// NaN/negative channels use zero; positive infinity and values above 65504
// use 65504. Peaks outside 400-2000 nits fail black.
[[nodiscard]] LinearRgb MapAgxToBt2020Nits(
    LinearRgb sceneLinearBt709, double displayPeakNits) noexcept;

}  // namespace fo4hdr
