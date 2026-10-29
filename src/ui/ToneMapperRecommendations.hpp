// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"

namespace fo4hdr {

struct ToneMapperRecommendedRange
{
    bool available{};
    // testedPeakNits is the HDR reference peak; SDR ranges have none.
    double minimum{}, maximum{}, testedPeakNits{};
};

[[nodiscard]] ToneMapperRecommendedRange GetToneMapperRecommendedRange(
    ProductionToneMapperAlgorithm algorithm, double requestedPeakNits,
    double ToneMapperParameters::* parameter) noexcept;

// SDR configurations have fixed whites, so their ranges need no peak.
[[nodiscard]] ToneMapperRecommendedRange GetSdrToneMapperRecommendedRange(
    ProductionToneMapperAlgorithm algorithm,
    double ToneMapperParameters::* parameter) noexcept;

} // namespace fo4hdr
