// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <type_traits>

namespace fo4hdr {

struct ImageSpaceActiveData
{
    float eyeAdaptSpeed{};
    float bloomBlurRadius{};
    float bloomThreshold{};
    float bloomScale{};
    float receiveBloomThreshold{};
    float white{};
    float sunlightScale{};
    float skyScale{};
    float eyeAdaptStrength{};
    float cinematicSaturation{};
    float cinematicBrightness{};
    float cinematicContrast{};
};

static_assert(std::is_trivially_copyable_v<ImageSpaceActiveData>);

}  // namespace fo4hdr
