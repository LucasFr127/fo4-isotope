// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fo4hdr {

inline constexpr double kDefaultUiPaperWhiteNits{203.0};
inline constexpr double kDefaultVideoWhiteNits{203.0};

struct HdrSettings
{
    double displayPeakNits{1000.0};
    double uiPaperWhiteNits{kDefaultUiPaperWhiteNits};
    double videoWhiteNits{kDefaultVideoWhiteNits};
};

enum class HdrSettingsValidationError : std::uint8_t
{
    none,
    displayPeakOutOfRange,
    uiPaperWhiteOutOfRange,
    videoWhiteOutOfRange
};

[[nodiscard]] HdrSettingsValidationError ValidateHdrSettings(
    const HdrSettings& settings) noexcept;

}  // namespace fo4hdr
