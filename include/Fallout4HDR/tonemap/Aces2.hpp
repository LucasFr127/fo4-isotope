// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <memory>

namespace fo4hdr {

class Aces2Transform;
struct Aces2TransformDeleter
{
    void operator()(const Aces2Transform* transform) const noexcept;
};
using Aces2TransformHandle = std::unique_ptr<const Aces2Transform, Aces2TransformDeleter>;

// Retain one immutable peak-specific context across a reference sample loop.
// Invalid peaks or allocation failure return an empty handle.
[[nodiscard]] Aces2TransformHandle CreateAces2Transform(double displayPeakNits) noexcept;
// Retains formation and Academy gamut compression, deferring final per-channel containment.
[[nodiscard]] LinearRgb EvaluateAces2LutNits(
    const Aces2Transform& transform, LinearRgb sceneLinearBt709) noexcept;
[[nodiscard]] LinearRgb MapAces2ToBt2020Nits(
    const Aces2Transform& transform, LinearRgb sceneLinearBt709) noexcept;

// J2M2 AP0/D60 look blended before AP1 formation. Strength zero bypasses only
// the look: the fixed zero-toe shadow blend and continuous gamut shoulder remain.
// Input is dimensionless scene-linear BT.709/D65; output is absolute BT.2020 nits.
// Invalid strength fails black. LUT values defer final per-channel containment.
[[nodiscard]] LinearRgb EvaluateAces2LmtLutNits(
    const Aces2Transform& transform, LinearRgb sceneLinearBt709,
    double lookStrength = 1.0) noexcept;
[[nodiscard]] LinearRgb MapAces2LmtToBt2020Nits(
    const Aces2Transform& transform, LinearRgb sceneLinearBt709,
    double lookStrength = 1.0) noexcept;

// Dimensionless scene-linear BT.709/D65 preserves the Academy 0.18 gray anchor.
// Returns contained absolute display-linear BT.2020 nits, without display encoding.
// Negative/nonfinite channels use zero; peaks outside 400-2000 nits fail black.
// A private per-thread cache prepares the peak-specific tables on first use or
// a peak change; this reference entry point is not a render hot-path contract.
[[nodiscard]] LinearRgb MapAces2ToBt2020Nits(
    LinearRgb sceneLinearBt709, double displayPeakNits) noexcept;

}  // namespace fo4hdr
