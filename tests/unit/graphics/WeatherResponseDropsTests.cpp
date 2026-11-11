// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "TestHarness.hpp"
#include "WeatherResponseDrops.hpp"

#include <array>
#include <vector>

namespace fo4hdr::tests {
namespace {

constexpr const char* kStale = "stale";
constexpr const char* kRoom = "room";

std::vector<WeatherResponseDrops::Entry> Entries(const WeatherResponseDrops& drops)
{
    std::vector<WeatherResponseDrops::Entry> entries;
    drops.ForEach(
        [&entries](const WeatherResponseDrops::Entry& entry) { entries.push_back(entry); });
    return entries;
}

void TestBriefAndLongDrops()
{
    WeatherResponseDrops drops;
    // A scene that never had the response has nothing to drop.
    drops.Observe(1, kRoom);
    FO4HDR_CHECK(Entries(drops).empty());

    drops.Observe(2, nullptr);
    drops.Observe(3, kStale);
    // Later reasons inside one gap belong to the drop that opened it.
    drops.Observe(4, kRoom);
    drops.Observe(5, nullptr);
    drops.Observe(6, kStale);
    drops.Observe(6 + WeatherResponseDrops::kBriefFrames, nullptr);
    drops.Observe(100, kStale);
    drops.Observe(101 + WeatherResponseDrops::kBriefFrames, nullptr);
    drops.Observe(200, kRoom);

    const auto entries = Entries(drops);
    FO4HDR_CHECK(entries.size() == 2);
    FO4HDR_CHECK(entries[0].reason == kStale);
    FO4HDR_CHECK(entries[0].drops == 3);
    FO4HDR_CHECK(entries[0].brief == 2);
    FO4HDR_CHECK(entries[0].longestBriefFrames == WeatherResponseDrops::kBriefFrames);
    FO4HDR_CHECK(entries[1].reason == kRoom);
    FO4HDR_CHECK(entries[1].drops == 1);
    FO4HDR_CHECK(entries[1].brief == 0);
    FO4HDR_CHECK(drops.Unlisted() == 0);
}

void TestForgetAndCapacity()
{
    WeatherResponseDrops drops;
    drops.Observe(1, nullptr);
    drops.Forget();
    // Turning the response off is not a drop, and its return is not a recovery.
    drops.Observe(2, kStale);
    drops.Observe(3, nullptr);
    FO4HDR_CHECK(Entries(drops).empty());

    std::array<char, WeatherResponseDrops::kCapacity + 1> reasons{};
    WeatherResponseDrops::Frame frame = 10;
    for (const auto& reason : reasons)
    {
        drops.Observe(frame++, &reason);
        drops.Observe(frame++, nullptr);
    }
    FO4HDR_CHECK(Entries(drops).size() == WeatherResponseDrops::kCapacity);
    FO4HDR_CHECK(drops.Unlisted() == 1);
}

}  // namespace

void TestWeatherResponseDrops()
{
    TestBriefAndLongDrops();
    TestForgetAndCapacity();
}

}  // namespace fo4hdr::tests
