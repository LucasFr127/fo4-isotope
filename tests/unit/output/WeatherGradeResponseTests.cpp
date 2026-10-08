// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "FO4Isotope/output/WeatherGradeResponse.hpp"
#include "TestHarness.hpp"

#include <limits>
#include <thread>
#include <vector>

namespace fo4hdr::tests {
namespace {

constexpr std::array<WeatherGradeAggregate, 2> kCacheCatalog{{
    {{0.75, 0.6, 0.35, 0.25}, 3, 3, WeatherGradeAvailability::complete, 1},
    {{0.8, 0.7, 0.2, 0.1}, 5, 5, WeatherGradeAvailability::complete, 2}}};

void TestWeatherCacheRefresh()
{
    WeatherGradeResponseCache cache;
    WeatherGradeResponseSettings settings;
    ResolvedLutGrade result;
    FO4HDR_CHECK(!cache.TryResolve(kCacheCatalog[0], result));
    FO4HDR_CHECK(cache.Refresh(kCacheCatalog, settings));
    for (const auto& aggregate : kCacheCatalog)
    {
        FO4HDR_CHECK(cache.TryResolve(aggregate, result));
        FO4HDR_CHECK(result == EvaluateWeatherGradeResponse(settings, aggregate.features, {}));
    }
    for (unsigned draw = 0; draw < 32; ++draw)
    {
        FO4HDR_CHECK(!cache.Refresh(kCacheCatalog, settings));
        FO4HDR_CHECK(cache.TryResolve(kCacheCatalog[1], result));
    }
    settings.luminance.gain = 0;
    settings.luminance.base = 23;
    FO4HDR_CHECK(!cache.Matches(kCacheCatalog, settings));
    FO4HDR_CHECK(cache.Refresh(kCacheCatalog, settings));
    FO4HDR_CHECK(cache.TryResolve(kCacheCatalog[1], result));
    FO4HDR_CHECK(result.luminancePercent == 23);
    FO4HDR_CHECK(!cache.Refresh(kCacheCatalog, settings));

    settings.enabled = false;
    FO4HDR_CHECK(cache.Refresh(kCacheCatalog, settings));
    FO4HDR_CHECK(!cache.TryResolve(kCacheCatalog[0], result));
    settings.enabled = true;
    FO4HDR_CHECK(cache.Refresh(kCacheCatalog, settings));
    FO4HDR_CHECK(cache.TryResolve(kCacheCatalog[0], result));
    settings = {};
    FO4HDR_CHECK(cache.Refresh(kCacheCatalog, settings));
    FO4HDR_CHECK(cache.TryResolve(kCacheCatalog[0], result));
    FO4HDR_CHECK(result == EvaluateWeatherGradeResponse(settings, kCacheCatalog[0].features, {}));
}

void TestWeatherCacheInvalidation()
{
    WeatherGradeResponseCache cache;
    WeatherGradeResponseSettings settings;
    ResolvedLutGrade result;
    FO4HDR_CHECK(cache.Refresh(kCacheCatalog, settings));
    auto wrong = kCacheCatalog[0];
    wrong.cacheIndex = 0;
    FO4HDR_CHECK(!cache.TryResolve(wrong, result));
    wrong.cacheIndex = 3;
    FO4HDR_CHECK(!cache.TryResolve(wrong, result));
    wrong = kCacheCatalog[0];
    ++wrong.referenceCount;
    FO4HDR_CHECK(!cache.TryResolve(wrong, result));

    auto changed = kCacheCatalog;
    changed[0].features.sunlight = 0.1;
    FO4HDR_CHECK(cache.Refresh(changed, settings));
    FO4HDR_CHECK(!cache.TryResolve(kCacheCatalog[0], result));
    FO4HDR_CHECK(cache.TryResolve(changed[0], result));
    FO4HDR_CHECK(result == EvaluateWeatherGradeResponse(settings, changed[0].features, {}));

    settings.color.gain = std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(!cache.Refresh(changed, settings));
    FO4HDR_CHECK(!cache.TryResolve(changed[0], result));
    settings = {};
    const std::array incomplete{kCacheCatalog[0], WeatherGradeAggregate{}};
    FO4HDR_CHECK(!cache.Refresh(incomplete, settings));
    FO4HDR_CHECK(!cache.TryResolve(kCacheCatalog[0], result));
    FO4HDR_CHECK(cache.Refresh({}, settings));
    FO4HDR_CHECK(!cache.TryResolve(kCacheCatalog[0], result));
}

void TestWeatherCacheCapacity()
{
    std::vector<WeatherGradeAggregate> catalog(kMaximumWeatherGradeImageSpaces, kCacheCatalog[0]);
    for (std::size_t index = 0; index < catalog.size(); ++index)
        catalog[index].cacheIndex = static_cast<std::uint32_t>(index + 1);
    WeatherGradeResponseCache cache;
    ResolvedLutGrade result;
    FO4HDR_CHECK(cache.Refresh(catalog, {}));
    FO4HDR_CHECK(cache.TryResolve(catalog.back(), result));
    FO4HDR_CHECK(result == EvaluateWeatherGradeResponse({}, catalog.back().features, {}));
    catalog.push_back(kCacheCatalog[0]);
    FO4HDR_CHECK(!cache.Refresh(catalog, {}));
    FO4HDR_CHECK(!cache.TryResolve(catalog.front(), result));
    FO4HDR_CHECK(!PublishWeatherGradeCatalog(catalog));
}

void TestWeatherCachePublication()
{
    FO4HDR_CHECK(ReadWeatherGradeCatalog().empty());
    const std::array invalid{WeatherGradeAggregate{}};
    FO4HDR_CHECK(!PublishWeatherGradeCatalog(invalid));
    auto source = kCacheCatalog;
    bool succeeded{};
    std::thread producer([&] { succeeded = PublishWeatherGradeCatalog(source); });
    producer.join();
    FO4HDR_CHECK(succeeded);
    source[0] = {};
    const auto published = ReadWeatherGradeCatalog();
    FO4HDR_CHECK(published.size() == kCacheCatalog.size());
    FO4HDR_CHECK(published[0] == kCacheCatalog[0]);
    FO4HDR_CHECK(published[1] == kCacheCatalog[1]);
    FO4HDR_CHECK(!PublishWeatherGradeCatalog(kCacheCatalog));
    FO4HDR_CHECK(ReadWeatherGradeCatalog().data() == published.data());
}

}  // namespace

void TestWeatherGradeResponse()
{
    WeatherGradeResponseSettings settings{};
    constexpr ResolvedLutGrade global{67.0, 82.0};
    constexpr WeatherGradeFeatures features{0.75, 0.6, 0.35, 0.25};
    const auto result = EvaluateWeatherGradeResponse(settings, features, global);
    FO4HDR_CHECK(test::Approx(result.luminancePercent, 52.11834505, 1.0e-12));
    FO4HDR_CHECK(test::Approx(result.colorPercent, 52.13001295, 1.0e-12));
    FO4HDR_CHECK(EvaluateWeatherGradeResponse(settings, {}, global) == ResolvedLutGrade{});
    const auto suppressed = EvaluateWeatherGradeResponse(settings, {1.0, 1.0, 0.0, 1.0}, global);
    FO4HDR_CHECK(suppressed.luminancePercent == 0.0);
    FO4HDR_CHECK(suppressed.colorPercent == 0.0);

    settings.luminance = {50.0, 1.0, 0.0, 100.0, 120.0, {}};
    settings.color = {35.0, 0.0, 20.0, 90.0, -500.0, {}};
    const auto shifted = EvaluateWeatherGradeResponse(settings, features, global);
    FO4HDR_CHECK(shifted.luminancePercent == 70.0);
    FO4HDR_CHECK(shifted.colorPercent == 35.0);
    settings.luminance.minimum = 80.0;
    FO4HDR_CHECK(EvaluateWeatherGradeResponse(settings, features, global).luminancePercent == 80.0);
    settings.luminance.maximum = 79.0;
    FO4HDR_CHECK(!ValidateWeatherGradeResponseSettings(settings));
    FO4HDR_CHECK(EvaluateWeatherGradeResponse(settings, features, global) == global);

    settings = {};
    settings.enabled = false;
    FO4HDR_CHECK(EvaluateWeatherGradeResponse(settings, features, global) == global);
    settings.enabled = true;
    auto invalidFeatures = features;
    invalidFeatures.ambient = std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(!ValidateWeatherGradeFeatures(invalidFeatures));
    FO4HDR_CHECK(EvaluateWeatherGradeResponse(settings, invalidFeatures, global) == global);
    settings.color.coefficients[0] = std::numeric_limits<double>::infinity();
    FO4HDR_CHECK(!ValidateWeatherGradeChannel(settings.color));
    FO4HDR_CHECK(EvaluateWeatherGradeResponse(settings, features, global) == global);
    TestWeatherCacheRefresh();
    TestWeatherCacheInvalidation();
    TestWeatherCacheCapacity();
    TestWeatherCachePublication();
}

}  // namespace fo4hdr::tests
