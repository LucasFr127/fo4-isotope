// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <memory>

namespace fo4hdr {

// Immutable, peak-specific ACES 2.0 forward Output Transform state. Its
// tables are generated once outside the render hot path and reused for every
// operator-LUT sample.
class Aces2Transform;

struct Aces2TransformDeleter
{
    void operator()(const Aces2Transform* transform) const noexcept;
};

using Aces2TransformHandle =
    std::unique_ptr<const Aces2Transform, Aces2TransformDeleter>;

// Uses the pinned Academy boundary model.
[[nodiscard]] Aces2TransformHandle CreateAces2Transform(
    double displayPeakNits) noexcept;

// Input is finite, non-negative, dimensionless scene-linear BT.709. The RGB
// primary/white-point adapter preserves neutral scene values, so 0.18 reaches
// the official ACES 0.18 middle-gray anchor without an absolute-nit rescale.
// Output is absolute display-linear BT.2020 nits. Display encoding, ST 2084,
// and UI scaling are deliberately not part of this operator.
[[nodiscard]] LinearRgb MapAces2ToBt2020Nits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709) noexcept;

// LUT generation only: retain finite gamut-fit excursions below zero/above
// the display peak so vertex containment does not bias interpolation. The
// LUT consumer must contain its interpolated result to [0, displayPeakNits].
// This is not a presentation-safe operator result.
[[nodiscard]] LinearRgb EvaluateAces2LutNits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709) noexcept;

// The pinned "J curve 2 + M curve 2" ACES-to-ACES Contrast LMT is applied in
// AP0/D60 before a local ACES 2 shadow adaptation. Look strength blends the
// AP0 input and LMT result; zero bypasses only the Look. The fixed adaptation
// blends toward a recomputed zero-toe scalar response below internal 0.18,
// before chroma/gamut processing. A fixed-width gamut shoulder removes the
// reference reach/gamut crossing discontinuity. Direct Academy APIs stay unchanged.
// Input/output units and containment rules otherwise match the direct ACES2
// functions. Invalid strength fails black.
[[nodiscard]] LinearRgb EvaluateAces2LmtLutNits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength = 1.0) noexcept;

[[nodiscard]] LinearRgb MapAces2LmtToBt2020Nits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength = 1.0) noexcept;

// Direct AP0/D60 scene boundary for the pinned Contrast LMT.
// This bypasses the BT.709 adapter, but retains the official AP1 formation
// clamp. Finite signed AP0 is accepted; non-finite components become zero.
// The LUT-only result retains finite display-gamut excursions.
[[nodiscard]] LinearRgb EvaluateAces2Ap0LutNits(
    const Aces2Transform& transform,
    LinearRgb sceneLinearAp0) noexcept;

// Diagnostic context-specific geometry, independent of LUT interpolation error.
// The reference RemapM discontinuity is gamutMinusReach == 0 with positive
// chromaMinusGamut. Production is continuous across this surface. Other
// branches/degenerate geometry return valid == false.
struct Aces2CompressionBoundary
{
    double gamutMinusReach{};
    double chromaMinusGamut{};
    bool valid{};
};

// Diagnostic boundary after the contrast LMT and local shadow tone scale. This is
// test/diagnostic geometry only; production evaluates the combined transform
// while baking the ACES2LMT coefficient lattice. Invalid strength returns an
// invalid boundary.
[[nodiscard]] Aces2CompressionBoundary EvaluateAces2LmtCompressionBoundary(
    const Aces2Transform& transform,
    LinearRgb sceneLinearBt709,
    double lookStrength = 1.0) noexcept;

// Convenience reference wrapper. Production LUT generation should retain the
// peak-specific context returned by CreateAces2Transform.
[[nodiscard]] LinearRgb MapAces2ToBt2020Nits(
    LinearRgb sceneLinearBt709,
    double displayPeakNits) noexcept;

}  // namespace fo4hdr
