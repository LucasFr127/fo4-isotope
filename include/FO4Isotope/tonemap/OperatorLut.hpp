// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"

#include <cstdint>
#include <atomic>
#include <array>
#include <vector>

namespace fo4hdr {

// Measured HDR logical grids, starting at 64 per edge. AgX's is the smallest at
// which every AgX control keeps its full recommended range, and GT7 qualifies at
// the minimum. ACES2's smaller qualifying edges, 87 and 91, sit within 0.08 of
// its cap.
inline constexpr std::uint32_t kAces2OperatorLutEdge{94};
inline constexpr std::uint32_t kAgxOperatorLutEdge{66};
inline constexpr std::uint32_t kGt7OperatorLutEdge{64};
// Smallest measured SDR grids, starting at 32 per edge, at which every SDR
// control keeps its full recommended range.
inline constexpr std::uint32_t kSdrAces2OperatorLutEdge{58};
inline constexpr std::uint32_t kSdrAgxOperatorLutEdge{49};
inline constexpr std::uint32_t kSdrGt7OperatorLutEdge{53};
inline constexpr std::uint32_t kOperatorLutCoefficientPadding{2};
inline constexpr double kOperatorLutSceneMaximum{65504.0};

[[nodiscard]] std::uint32_t OperatorLutEdge(
    ProductionToneMapperAlgorithm algorithm) noexcept;

[[nodiscard]] std::uint32_t SdrOperatorLutEdge(
    ProductionToneMapperAlgorithm algorithm) noexcept;

[[nodiscard]] double OperatorLutInputOffset(
    ProductionToneMapperAlgorithm algorithm) noexcept;

// GPU coefficients: angle gain, bias, log center, logarithmic gain.
[[nodiscard]] std::array<float, 4> OperatorLutShaperParameters(
    ProductionToneMapperAlgorithm algorithm, double displayPeakNits) noexcept;

enum class OperatorLutTarget : std::uint8_t
{
    hdr10,
    sdr
};

// Fixed white of each operator's SDR configuration in nits: ACES2 100, AgX
// 203 and GT7 250. Zero for any other algorithm.
[[nodiscard]] double SdrOperatorWhiteNits(
    ProductionToneMapperAlgorithm algorithm) noexcept;

// Owns tightly packed RGBA16F texels in D3D11 Texture3D order (x changes
// fastest, then y, then z). Tables consume scene-linear BT.709. HDR10 tables
// store uncontained absolute display-linear BT.2020 nits; SDR tables store
// uncontained relative display-linear BT.709 with white at 1, and their
// displayPeakNits is the SDR white. Sampling must apply the final containment
// AFTER interpolation. Negative lattice values and values above peak preserve
// the reference's boundary slopes. No PQ.
struct OperatorLutData
{
    std::vector<std::uint16_t> rgba16;
    // One coefficient ghost on each side makes textureEdge equal edge+2.
    std::uint32_t edge{};
    std::uint32_t textureEdge{};
    double inputShaperMinimum{};
    double inputShaperMaximum{};
    double inputOffset{};
    double displayPeakNits{};
    ProductionToneMapperAlgorithm algorithm{
        ProductionToneMapperAlgorithm::aces2};
    OperatorLutTarget target{OperatorLutTarget::hdr10};
    ToneMapperParameters toneMapperParameters{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct OperatorLutCancellation
{
    const std::atomic<std::uint64_t>* generation{};
    std::uint64_t expectedGeneration{};

    [[nodiscard]] bool IsCancelled() const noexcept
    {
        return generation != nullptr &&
            generation->load(std::memory_order_acquire) != expectedGeneration;
    }
};

// The coordinate mapping matches OperatorLut.hlsli.
// Every operator covers the entire nonnegative finite FP16 scene input.
// Initial generation finishes before graphics-hook installation. Device
// recreation uploads the retained CPU data. Live changes use the cancellable
// CPU worker, never the render path.
[[nodiscard]] OperatorLutData BuildOperatorLut(
    ProductionToneMapperAlgorithm algorithm,
    double displayPeakNits,
    const ToneMapperParameters& parameters = {},
    OperatorLutCancellation cancellation = {}) noexcept;

// The operator's SDR configuration at its fixed white.
[[nodiscard]] OperatorLutData BuildSdrOperatorLut(
    ProductionToneMapperAlgorithm algorithm,
    const ToneMapperParameters& parameters = kDefaultSdrToneMapperParameters,
    OperatorLutCancellation cancellation = {}) noexcept;

// Final containment of an interpolated SDR sample. ACES2 clamps each channel
// to [0, 1] like its Rec.709 preset. AgX soft-clips each channel at zero and
// scales back to its BT.709 luminance, then contracts chroma toward that
// luminance with a 5% knee until the brightest channel fits. GT7 clips each
// BT.2020 channel at white like its official sample, then contracts BT.709
// chroma toward luminance with the same knee until it fits. Both keep a color's
// BT.709 luminance below white. Nonfinite input fails black.
[[nodiscard]] LinearRgb ContainSdrOperatorOutput(
    ProductionToneMapperAlgorithm algorithm, LinearRgb relativeBt709) noexcept;

[[nodiscard]] double DecodeOperatorLutCoordinate(
    double coordinate,
    ProductionToneMapperAlgorithm algorithm,
    double displayPeakNits) noexcept;

[[nodiscard]] double EncodeOperatorLutCoordinate(
    double value,
    ProductionToneMapperAlgorithm algorithm,
    double displayPeakNits) noexcept;

[[nodiscard]] double OperatorLutInputShaperMinimum(
    ProductionToneMapperAlgorithm algorithm,
    double displayPeakNits) noexcept;

[[nodiscard]] double OperatorLutInputShaperMaximum(
    ProductionToneMapperAlgorithm algorithm,
    double displayPeakNits) noexcept;

}  // namespace fo4hdr
