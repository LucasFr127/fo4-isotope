// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <cmath>

namespace fo4hdr {
namespace {

bool InRange(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

}  // namespace

HdrSettingsValidationError ValidateHdrSettings(const HdrSettings& settings) noexcept
{
    if (!InRange(settings.displayPeakNits, kMinimumDisplayPeakNits, kMaximumDisplayPeakNits))
    {
        return HdrSettingsValidationError::displayPeakOutOfRange;
    }
    if (!InRange(settings.uiPaperWhiteNits, kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits))
    {
        return HdrSettingsValidationError::uiPaperWhiteOutOfRange;
    }
    if (!InRange(settings.videoWhiteNits, kMinimumUiPaperWhiteNits, kMaximumUiPaperWhiteNits))
    {
        return HdrSettingsValidationError::videoWhiteOutOfRange;
    }
    return HdrSettingsValidationError::none;
}

}  // namespace fo4hdr
