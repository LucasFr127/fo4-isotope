// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

namespace fo4hdr {

// Linear Rec.2020 framebuffer input and output use 1.0 = 100 cd/m².
// Both scalar interfaces require finite peaks in [250,10000] nits and fix
// toe 1.280, shoulder start 0.444 and color blend 0.6. NaN/nonpositive inputs
// use zero; positive infinity and inputs above 65504 use 65504.
[[nodiscard]] LinearRgb MapGt7ReferenceRec2020(
    LinearRgb framebufferRec2020, double displayPeakNits) noexcept;

// Scene-linear BT.709/D65 uses the fixed 203-nit reference white. Returns
// contained absolute display-linear BT.2020 nits, without display encoding.
[[nodiscard]] LinearRgb MapGt7ToBt2020Nits(
    LinearRgb sceneLinearBt709, double displayPeakNits) noexcept;

}  // namespace fo4hdr
