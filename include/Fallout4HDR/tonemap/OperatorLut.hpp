// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <cstdint>
#include <atomic>
#include <array>
#include <vector>

namespace fo4hdr {

// Smallest qualified coefficient grids; the smooth shaper also resolves shadows.
inline constexpr std::uint32_t kAces2OperatorLutEdge{128};
inline constexpr std::uint32_t kAgxOperatorLutEdge{92};
inline constexpr std::uint32_t kGt7OperatorLutEdge{64};
inline constexpr std::uint32_t kOperatorLutCoefficientPadding{2};
inline constexpr double kOperatorLutSceneMaximum{65504.0};

[[nodiscard]] std::uint32_t OperatorLutEdge(
    ProductionToneMapperAlgorithm algorithm) noexcept;

[[nodiscard]] double OperatorLutInputOffset(
    ProductionToneMapperAlgorithm algorithm) noexcept;

// GPU coefficients: angle gain, bias, log center, logarithmic gain.
[[nodiscard]] std::array<float, 4> OperatorLutShaperParameters(
    ProductionToneMapperAlgorithm algorithm, double displayPeakNits) noexcept;

// Owns tightly packed RGBA16F texels in D3D11 Texture3D order (x changes
// fastest, then y, then z). Tables consume scene-linear BT.709 and store
// uncontained absolute display-linear BT.2020 nits. Sampling must apply the
// final [0, peak] containment AFTER interpolation. Negative lattice values
// and values above peak preserve the reference's boundary slopes. No PQ.
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
        ProductionToneMapperAlgorithm::aces2Lmt};
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
