// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <memory>

namespace fo4hdr {

class Gt7Transform;
struct Gt7TransformDeleter
{
    void operator()(const Gt7Transform* transform) const noexcept;
};
using Gt7TransformHandle = std::unique_ptr<const Gt7Transform, Gt7TransformDeleter>;

// Retain one immutable peak-specific context across a reference sample loop.
// Invalid peaks/selected controls, preparation or allocation return an empty handle.
[[nodiscard]] Gt7TransformHandle CreateGt7Transform(
    double displayPeakNits, const ToneMapperParameters& parameters = {}) noexcept;
// Scene input policy and source-order operator math precede final display containment.
[[nodiscard]] LinearRgb EvaluateGt7LutNits(
    const Gt7Transform& transform, LinearRgb sceneLinearBt709) noexcept;
[[nodiscard]] LinearRgb MapGt7ReferenceRec2020(
    const Gt7Transform& transform, LinearRgb framebufferRec2020) noexcept;
[[nodiscard]] LinearRgb MapGt7ToBt2020Nits(
    const Gt7Transform& transform, LinearRgb sceneLinearBt709) noexcept;

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
