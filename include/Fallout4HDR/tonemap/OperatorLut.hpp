// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace fo4hdr {

// Packed RGBA16F cubic coefficients, x fastest, then y, then z. Input is
// dimensionless scene-linear BT.709; values are uncontained BT.2020 nits.
// Signed and above-peak coefficients preserve boundary slopes. Containment
// belongs after reconstruction, and display encoding is not stored here.
struct OperatorLutData
{
    std::vector<std::uint16_t> rgba16;
    std::uint32_t edge{};
    std::uint32_t textureEdge{};
    double displayPeakNits{};
    OperatorLutAlgorithm algorithm{OperatorLutAlgorithm::aces2};
    ToneMapperParameters parameters{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct OperatorLutCancellation
{
    // A non-null counter must outlive the synchronous build.
    const std::atomic<std::uint64_t>* generation{};
    std::uint64_t expectedGeneration{};

    [[nodiscard]] bool IsCancelled() const noexcept
    {
        return generation != nullptr &&
            generation->load(std::memory_order_acquire) != expectedGeneration;
    }
};

// Ordered GPU constants: angle gain, bias, logarithm center, logarithm gain.
// Invalid algorithms or peaks return zero constants.
[[nodiscard]] std::array<float, 4> OperatorLutShaperParameters(
    OperatorLutAlgorithm algorithm, double displayPeakNits) noexcept;

// Cold CPU-only preparation. Invalid algorithms/peaks/selected controls, cancellation, allocation
// or nonfinite/FP16-overflow coefficients return an invalid empty result.
[[nodiscard]] OperatorLutData BuildOperatorLut(
    OperatorLutAlgorithm algorithm,
    double displayPeakNits,
    OperatorLutCancellation cancellation = {}) noexcept;

[[nodiscard]] OperatorLutData BuildOperatorLut(
    OperatorLutAlgorithm algorithm,
    double displayPeakNits,
    const ToneMapperParameters& parameters,
    OperatorLutCancellation cancellation) noexcept;

}  // namespace fo4hdr
