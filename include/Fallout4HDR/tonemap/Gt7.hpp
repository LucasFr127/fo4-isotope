// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/config/HdrSettings.hpp"

#include <memory>

namespace fo4hdr {

class Gt7Transform;
struct Gt7TransformDeleter
{
    void operator()(const Gt7Transform* transform) const noexcept;
};
using Gt7TransformHandle =
    std::unique_ptr<const Gt7Transform, Gt7TransformDeleter>;

// Retain one immutable context per LUT bake; invalid settings fail creation.
[[nodiscard]] Gt7TransformHandle CreateGt7Transform(
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

[[nodiscard]] LinearRgb EvaluateGt7LutNits(
    const Gt7Transform& transform, LinearRgb sceneLinear) noexcept;

// Preserves the official HDR/ICtCp input and output contract: linear Rec.2020
// framebuffer values where 1.0 represents 100 cd/m². Valid peaks are 250-10000.
[[nodiscard]] LinearRgb MapGt7ReferenceRec2020(
    LinearRgb framebufferRec2020,
    double displayPeakNits) noexcept;

// Scene-linear BT.709 uses a fixed 203-nit reference. The native Rec.2020
// result is published as contained absolute linear BT.2020 nits. UI white
// remains independent.
[[nodiscard]] LinearRgb MapGt7ToBt2020Nits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

// LUT-bake intermediate only. The official final peak clamp is deferred to
// sampling, so interpolation does not smooth across an already clipped knee.
// This is not a composition-ready output or a different artistic operator.
[[nodiscard]] LinearRgb EvaluateGt7LutNits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

// Comparison oracle only. Never use this inverse-BT.709 gamut fit in the
// production shader graph.
[[nodiscard]] LinearRgb MapGt7ToBt709NitsM12(
    LinearRgb sceneLinear,
    double displayPeakNits) noexcept;

}  // namespace fo4hdr
