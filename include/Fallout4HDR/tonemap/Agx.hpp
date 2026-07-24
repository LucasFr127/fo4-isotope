// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <optional>

namespace fo4hdr {

class AgxTransform
{
public:
    struct SigmoidSegment
    {
        double scale;
        double power;
    };
    struct Sigmoid
    {
        double transitionX;
        double transitionY;
        SigmoidSegment toe;
        SigmoidSegment shoulder;
        double slope;
        double intercept;
    };

private:
    friend std::optional<AgxTransform> CreateAgxTransform(
        double, const ToneMapperParameters&) noexcept;
    friend LinearRgb MapAgxToBt2020Nits(const AgxTransform&, LinearRgb) noexcept;
    friend LinearRgb EvaluateAgxLutNits(const AgxTransform&, LinearRgb) noexcept;
    AgxTransform() noexcept = default;

    double displayPeakNits_{};
    double hdrSdrRatio_{};
    double darkMaximumExponent_{};
    double hdrPurity_{};
    Sigmoid formationSigmoid_{};
    Sigmoid darkeningSigmoid_{};
};

// Small prepared value state avoids allocation and repeated sigmoid formation.
// Invalid peaks/selected controls or preparation return no context.
[[nodiscard]] std::optional<AgxTransform> CreateAgxTransform(
    double displayPeakNits, const ToneMapperParameters& parameters = {}) noexcept;
// Raw prepared operator output preserves slopes before display gamut containment.
[[nodiscard]] LinearRgb EvaluateAgxLutNits(
    const AgxTransform& transform, LinearRgb sceneLinearBt709) noexcept;
[[nodiscard]] LinearRgb MapAgxToBt2020Nits(
    const AgxTransform& transform, LinearRgb sceneLinearBt709) noexcept;

// Final AgX gamut containment also applies after coefficient reconstruction.
[[nodiscard]] LinearRgb ApplyAgxDisplayGamut(
    LinearRgb displayLinearBt2020Nits, double displayPeakNits) noexcept;

// Scene-linear BT.709/D65 uses the fixed 203-nit scene reference white.
// Returns contained absolute display-linear BT.2020 nits, without display encoding.
// Fixed AgX response: EV 0, contrast 2.7, HDR purity 0.5 and shoulder power 2.
// NaN/negative channels use zero; positive infinity and values above 65504
// use 65504. Peaks outside 400-2000 nits fail black.
[[nodiscard]] LinearRgb MapAgxToBt2020Nits(
    LinearRgb sceneLinearBt709, double displayPeakNits) noexcept;

}  // namespace fo4hdr
