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

bool AgxParametersValid(const ToneMapperParameters& parameters) noexcept
{
    return InRange(parameters.agxMiddleGrayEv, kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv) &&
        InRange(parameters.agxContrast, kMinimumAgxContrast, kMaximumAgxContrast) &&
        InRange(parameters.agxHdrPurity, kMinimumAgxHdrPurity, kMaximumAgxHdrPurity) &&
        InRange(parameters.agxHdrShoulderPower, kMinimumAgxHdrShoulderPower,
            kMaximumAgxHdrShoulderPower);
}

bool Gt7ParametersValid(const ToneMapperParameters& parameters) noexcept
{
    return InRange(parameters.gt7ToeStrength, kMinimumGt7ToeStrength, kMaximumGt7ToeStrength) &&
        InRange(parameters.gt7ShoulderStart, kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart) &&
        InRange(parameters.gt7ColorBlend, kMinimumGt7ColorBlend, kMaximumGt7ColorBlend);
}

}  // namespace

bool ValidateToneMapperParameters(
    OperatorLutAlgorithm algorithm, const ToneMapperParameters& parameters) noexcept
{
    switch (algorithm)
    {
    case OperatorLutAlgorithm::aces2: return true;
    case OperatorLutAlgorithm::agX:
        return AgxParametersValid(parameters);
    case OperatorLutAlgorithm::gt7:
        return Gt7ParametersValid(parameters);
    }
    return false;
}

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
    if (!InRange(settings.vanillaColorGradingStrengthPercent,
        kMinimumVanillaColorGradingPercent, kMaximumVanillaColorGradingPercent))
    {
        return HdrSettingsValidationError::vanillaColorGradingOutOfRange;
    }
    return HdrSettingsValidationError::none;
}

}  // namespace fo4hdr
