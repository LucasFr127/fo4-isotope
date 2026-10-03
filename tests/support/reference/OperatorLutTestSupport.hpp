// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"

#include <cstdint>
#include <vector>

namespace fo4hdr::test {

[[nodiscard]] double HalfToDouble(std::uint16_t bits) noexcept;

// Independently sum all 64 coefficient-lattice contributions from the actual
// half-precision texture rather than sharing the shader's grouped fetches.
[[nodiscard]] LinearRgb SampleOperatorLut(
    const OperatorLutData& lut, LinearRgb scene) noexcept;

// Maximum errors use two significant digits with positive ties upward; p99 and
// the one-unit potential-JND goal remain unrounded. BT.2124 Annex 4 reports 2.363
// as 2.4; using that precision and these caps for acceptance is project policy,
// not a standard invisibility threshold.
// https://www.itu.int/rec/R-REC-BT.2124-0-201901-I/en
[[nodiscard]] double OperatorLutMaximumDeltaEItp(
    ProductionToneMapperAlgorithm algorithm) noexcept;
inline constexpr double kOperatorLutPercentileDeltaEItp{1.0};
inline constexpr double kOperatorLutGoalDeltaEItp{1.0};

struct OperatorLutQuality
{
    bool available{};
    double maximumDeltaEItp{};
    double broadP99DeltaEItp{};
    double holdoutP99DeltaEItp{};
    std::uint32_t sampleCount{};

    [[nodiscard]] bool MeetsPracticalLimit(ProductionToneMapperAlgorithm algorithm) const noexcept;
    [[nodiscard]] bool MeetsFidelityGoal(ProductionToneMapperAlgorithm algorithm) const noexcept;
};

enum class OperatorLutQualityRegion : std::uint8_t
{
    broad,
    holdout,
    other
};

class OperatorLutQualityMeasurements
{
public:
    void Record(double error, OperatorLutQualityRegion region);
    [[nodiscard]] OperatorLutQuality Summary();

private:
    std::vector<double> broad_;
    std::vector<double> holdout_;
    double maximum_{};
    std::uint32_t sampleCount_{};
    bool invalid_{};
};

// Offline evaluation of actual half coefficients against the
// configured analytical response. Cancellation or invalid samples fail closed.
[[nodiscard]] OperatorLutQuality EvaluateOperatorLutQuality(
    const OperatorLutData& lut,
    OperatorLutCancellation cancellation = {}) noexcept;

// Binary32 input corpora are shared by offline qualification and CPU/GPU
// checks; holdout generation stays independent of LUT knots and shapers.
[[nodiscard]] std::vector<LinearRgb> BuildOperatorLutCorpus(double peak);
[[nodiscard]] std::vector<LinearRgb> BuildOperatorLutHoldoutCorpus();
[[nodiscard]] std::vector<LinearRgb> BuildNearBlackOperatorCorpus();

} // namespace fo4hdr::test
