// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/config/HdrSettings.hpp"

#include <optional>

namespace fo4hdr {

// Small immutable AgX state prepared once for a display peak and parameter
// set. Production LUT generation reuses it for every lattice sample.
class AgxTransform
{
public:
    struct SigmoidSegment
    {
        double scale{};
        double power{};
    };

    struct Sigmoid
    {
        double transitionX{};
        double transitionY{};
        SigmoidSegment toe{};
        SigmoidSegment shoulder{};
        double slope{};
        double intercept{};
    };

    [[nodiscard]] double DisplayPeakNits() const noexcept;
    [[nodiscard]] double HdrSdrRatio() const noexcept;
    [[nodiscard]] double DarkMaximumExponent() const noexcept;
    [[nodiscard]] double HdrPurity() const noexcept;
    [[nodiscard]] const Sigmoid& FormationSigmoid() const noexcept;
    [[nodiscard]] const Sigmoid& DarkeningSigmoid() const noexcept;

private:
    friend std::optional<AgxTransform> CreateAgxTransform(
        double, const ToneMapperParameters&) noexcept;

    AgxTransform() noexcept = default;

    double displayPeakNits_{};
    double hdrSdrRatio_{};
    double darkMaximumExponent_{};
    double hdrPurity_{};
    Sigmoid formationSigmoid_{};
    Sigmoid darkeningSigmoid_{};
};

[[nodiscard]] std::optional<AgxTransform> CreateAgxTransform(
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

// The production AgX adapter consumes scene-linear BT.709 and returns bounded
// absolute display-linear BT.2020 nits. It uses the fixed scene-reference white;
// UI paper white is a separate presentation input.
[[nodiscard]] LinearRgb MapAgxToBt2020Nits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

[[nodiscard]] LinearRgb MapAgxToBt2020Nits(
    const AgxTransform& transform,
    LinearRgb sceneLinear) noexcept;

// LUT-bake intermediate only: retains the pinned source's finite signed and
// over-peak result. The LUT consumer applies ApplyAgxDisplayGamut after
// interpolation; this result is not suitable for presentation.
[[nodiscard]] LinearRgb EvaluateAgxLutNits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

[[nodiscard]] LinearRgb EvaluateAgxLutNits(
    const AgxTransform& transform,
    LinearRgb sceneLinear) noexcept;

// Production display-gamut stage for an interpolated AgX result. Negative
// channels contract smoothly about BT.2020 luminance; upper highlights scale
// RGB uniformly to retain color ratios. Invalid input or peak fails black.
[[nodiscard]] LinearRgb ApplyAgxDisplayGamut(
    LinearRgb displayLinearBt2020Nits,
    double displayPeakNits) noexcept;

// Explicit pinned-source reference. This bypasses Fallout4HDR's production
// gamut containment and retains the source's final per-channel clamp.
[[nodiscard]] LinearRgb MapAgxSourceToBt2020Nits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

}  // namespace fo4hdr
