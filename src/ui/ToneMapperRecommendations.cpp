// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ToneMapperRecommendations.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fo4hdr {
namespace {

struct ReferencePeak
{
    double nits;
    double acesLookMinimum;
};

// Ascending peaks resolve equal distances toward the lower reference.
constexpr std::array kReferencePeaks{
    ReferencePeak{400.0, 0.0}, ReferencePeak{430.0, 0.0},
    ReferencePeak{600.0, 0.0}, ReferencePeak{800.0, 0.0},
    ReferencePeak{1000.0, 0.0}, ReferencePeak{1200.0, 0.0},
    ReferencePeak{1500.0, 0.98}, ReferencePeak{2000.0, 0.99}};

struct ControlRange
{
    ProductionToneMapperAlgorithm algorithm;
    double ToneMapperParameters::* parameter;
    double minimum, maximum;
};

// Measured independently with other controls at defaults; slider limits may later change.
constexpr std::array kControlRanges{
    ControlRange{ProductionToneMapperAlgorithm::aces2,
        &ToneMapperParameters::acesLookStrength, 0.0, 1.0},
    ControlRange{ProductionToneMapperAlgorithm::agX,
        &ToneMapperParameters::agxMiddleGrayEv, -0.5, 0.5},
    ControlRange{ProductionToneMapperAlgorithm::agX,
        &ToneMapperParameters::agxContrast, 2.2, 3.0},
    ControlRange{ProductionToneMapperAlgorithm::agX,
        &ToneMapperParameters::agxHdrPurity, 0.25, 0.6},
    ControlRange{ProductionToneMapperAlgorithm::agX,
        &ToneMapperParameters::agxHdrShoulderPower, 1.75, 2.25},
    ControlRange{ProductionToneMapperAlgorithm::gt7,
        &ToneMapperParameters::gt7ToeStrength, 1.0, 1.5},
    ControlRange{ProductionToneMapperAlgorithm::gt7,
        &ToneMapperParameters::gt7ShoulderStart, 0.35, 0.6},
    ControlRange{ProductionToneMapperAlgorithm::gt7,
        &ToneMapperParameters::gt7ColorBlend, 0.0, 1.0}};

} // namespace

ToneMapperRecommendedRange GetToneMapperRecommendedRange(
    ProductionToneMapperAlgorithm algorithm, double requestedPeakNits,
    double ToneMapperParameters::* parameter) noexcept
{
    if (!std::isfinite(requestedPeakNits)) return {};
    if (requestedPeakNits < kReferencePeaks.front().nits) return {};
    if (requestedPeakNits > kReferencePeaks.back().nits) return {};
    const auto control = std::find_if(kControlRanges.begin(), kControlRanges.end(),
        [=](const auto& range) {
            return range.algorithm == algorithm && range.parameter == parameter;
        });
    if (control == kControlRanges.end()) return {};
    const auto reference = std::min_element(kReferencePeaks.begin(), kReferencePeaks.end(),
        [=](const auto& left, const auto& right) {
            return std::abs(left.nits - requestedPeakNits) <
                std::abs(right.nits - requestedPeakNits);
        });
    const auto minimum = algorithm == ProductionToneMapperAlgorithm::aces2 ?
        reference->acesLookMinimum : control->minimum;
    return {true, minimum, control->maximum, reference->nits};
}

} // namespace fo4hdr
