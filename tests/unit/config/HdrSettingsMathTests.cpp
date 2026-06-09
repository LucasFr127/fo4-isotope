// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using fo4hdr::HdrSettings;
using Error = fo4hdr::HdrSettingsValidationError;

auto SettingBits(const HdrSettings& value)
{
    return std::array{std::bit_cast<std::uint64_t>(value.displayPeakNits),
        std::bit_cast<std::uint64_t>(value.uiPaperWhiteNits),
        std::bit_cast<std::uint64_t>(value.videoWhiteNits)};
}

void TestSettingsDefaults()
{
    const HdrSettings defaults{};
    FO4HDR_CHECK(defaults.displayPeakNits == 1000.0);
    FO4HDR_CHECK(defaults.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(defaults.videoWhiteNits == 203.0);
    FO4HDR_CHECK(fo4hdr::kSceneReferenceWhiteNits == 203.0);
    FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(defaults) == Error::none);
}

void TestSettingsRanges()
{
    struct Range
    {
        double HdrSettings::* member;
        double minimum;
        double maximum;
        Error error;
    };
    constexpr std::array ranges{
        Range{&HdrSettings::displayPeakNits, 400.0, 2000.0, Error::displayPeakOutOfRange},
        Range{&HdrSettings::uiPaperWhiteNits, 100.0, 300.0, Error::uiPaperWhiteOutOfRange},
        Range{&HdrSettings::videoWhiteNits, 100.0, 300.0, Error::videoWhiteOutOfRange}};
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto& range : ranges)
    {
        HdrSettings settings{};
        for (const auto endpoint : {range.minimum, range.maximum})
        {
            settings.*range.member = endpoint;
            FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == Error::none);
        }
        for (const auto invalid : {std::nextafter(range.minimum, -infinity),
                 std::nextafter(range.maximum, infinity), nan, infinity, -infinity})
        {
            settings.*range.member = invalid;
            const auto before = SettingBits(settings);
            FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == range.error);
            FO4HDR_CHECK(SettingBits(settings) == before);
        }
    }
}

void TestIndependentWhites()
{
    HdrSettings settings{};
    settings.uiPaperWhiteNits = 300.0;
    FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == Error::none);
    FO4HDR_CHECK(settings.videoWhiteNits == 203.0);
    FO4HDR_CHECK(settings.displayPeakNits == 1000.0);
    settings = {};
    settings.videoWhiteNits = 100.0;
    FO4HDR_CHECK(fo4hdr::ValidateHdrSettings(settings) == Error::none);
    FO4HDR_CHECK(settings.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(settings.displayPeakNits == 1000.0);
}

}  // namespace

namespace fo4hdr::tests {

void RunHdrSettingsMathTests()
{
    TestSettingsDefaults();
    TestSettingsRanges();
    TestIndependentWhites();
}

}  // namespace fo4hdr::tests
