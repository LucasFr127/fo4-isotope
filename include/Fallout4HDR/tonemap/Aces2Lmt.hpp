// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

namespace fo4hdr {

// Applies the pinned ACES Contrast LMT "J curve 2 + M curve 2", correcting
// the source's hue on the vertical opponent axis. Input and output are
// dimensionless scene-linear ACES2065-1 (AP0/D60).
// The LMT is a creative ACES-to-ACES transform: it performs no Output
// Transform, display-peak adaptation, gamut conversion, containment, or PQ
// encoding.
//
// Finite signed input is retained. Matching the source DCTL, cone responses
// use sign-preserving power functions; an achromatic response A <= 0 collapses
// to AP0 black. Any non-finite input or final output fails closed to
// black rather than allowing invalid data into LUT generation.
[[nodiscard]] LinearRgb ApplyAces2ContrastLmt(
    LinearRgb sceneLinearAp0) noexcept;

}  // namespace fo4hdr
