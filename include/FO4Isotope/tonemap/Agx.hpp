// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"

#include <memory>
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
    // The SDR configuration's color source; null for an HDR transform.
    [[nodiscard]] const AgxTransform* ColorReference() const noexcept;

private:
    friend std::optional<AgxTransform> CreateAgxTransform(
        double, const ToneMapperParameters&) noexcept;
    friend std::optional<AgxTransform> CreateSdrAgxTransform(
        const ToneMapperParameters&) noexcept;
    [[nodiscard]] static std::optional<AgxTransform> Prepare(
        double displayPeakNits, const ToneMapperParameters& parameters) noexcept;

    AgxTransform() noexcept = default;

    double displayPeakNits_{};
    double hdrSdrRatio_{};
    double darkMaximumExponent_{};
    double hdrPurity_{};
    Sigmoid formationSigmoid_{};
    Sigmoid darkeningSigmoid_{};
    std::shared_ptr<const AgxTransform> colorReference_;
};

[[nodiscard]] std::optional<AgxTransform> CreateAgxTransform(
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

// SDR configuration: an HDR/SDR ratio of 1, so the peak is the fixed scene
// reference white and Middle Gray can only fall (to 0 EV at most). The response
// keeps that configuration's BT.2020 luminance but takes its RGB ratios from the
// same settings at kAgxSdrColorReferencePeakNits, so SDR shows AgX's HDR color
// and HDR Color Response acts as in HDR. The reference keeps the default
// Shoulder Power, which has no SDR effect.
inline constexpr double kAgxSdrPeakNits{kSceneReferenceWhiteNits};
// The shipped default display peak.
inline constexpr double kAgxSdrColorReferencePeakNits{1000.0};
[[nodiscard]] std::optional<AgxTransform> CreateSdrAgxTransform(
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

// Explicit pinned-source reference. This bypasses FO4Isotope's production
// gamut containment and retains the source's final per-channel clamp.
[[nodiscard]] LinearRgb MapAgxSourceToBt2020Nits(
    LinearRgb sceneLinear,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {}) noexcept;

}  // namespace fo4hdr
