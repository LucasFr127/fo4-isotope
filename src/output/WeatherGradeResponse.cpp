// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/output/WeatherGradeResponse.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace fo4hdr {
namespace {

using WeatherGradeCatalog = std::vector<WeatherGradeAggregate>;
std::atomic<const WeatherGradeCatalog*> g_weatherGradeCatalog{};

bool InRange(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

double EvaluateChannel(const WeatherGradeChannel& channel,
    const WeatherGradeFeatures& features) noexcept
{
    const std::array inputs{features.sunlight, features.glare,
        features.ambient, features.colorChange};
    auto response = channel.intercept;
    for (std::size_t index = 0; index < inputs.size(); ++index)
        response += channel.coefficients[index] * inputs[index];
    return std::clamp(channel.base + channel.gain * (response - 100.0),
        channel.minimum, channel.maximum);
}

bool CompleteWeatherGradeAggregate(const WeatherGradeAggregate& aggregate) noexcept
{
    return aggregate.availability == WeatherGradeAvailability::complete &&
        aggregate.referenceCount != 0 &&
        aggregate.referenceCount == aggregate.validReferenceCount &&
        ValidateWeatherGradeFeatures(aggregate.features);
}

bool ValidWeatherGradeCatalog(std::span<const WeatherGradeAggregate> catalog) noexcept
{
    if (catalog.size() > kMaximumWeatherGradeImageSpaces) return false;
    std::uint32_t index{};
    return std::ranges::all_of(catalog, [&index](const WeatherGradeAggregate& aggregate) {
        return aggregate.cacheIndex == ++index && CompleteWeatherGradeAggregate(aggregate);
    });
}

}  // namespace

bool ValidateWeatherGradeFeatures(const WeatherGradeFeatures& features) noexcept
{
    return InRange(features.sunlight, 0.0, 1.0) &&
        InRange(features.glare, 0.0, 1.0) &&
        InRange(features.ambient, 0.0, 1.0) &&
        InRange(features.colorChange, -2.0, 2.0);
}

bool ValidateWeatherGradeChannel(const WeatherGradeChannel& channel) noexcept
{
    return InRange(channel.base, 0.0, 100.0) &&
        InRange(channel.gain, 0.0, kMaximumWeatherGradeGain) &&
        InRange(channel.minimum, 0.0, 100.0) &&
        InRange(channel.maximum, channel.minimum, 100.0) &&
        InRange(channel.intercept, -kMaximumWeatherGradeCoefficient,
            kMaximumWeatherGradeCoefficient) &&
        std::ranges::all_of(channel.coefficients, [](double value) {
            return InRange(value, -kMaximumWeatherGradeCoefficient,
                kMaximumWeatherGradeCoefficient);
        });
}

bool ValidateWeatherGradeResponseSettings(
    const WeatherGradeResponseSettings& settings) noexcept
{
    return ValidateWeatherGradeChannel(settings.luminance) &&
        ValidateWeatherGradeChannel(settings.color);
}

ResolvedLutGrade EvaluateWeatherGradeResponse(
    const WeatherGradeResponseSettings& settings,
    const WeatherGradeFeatures& features, ResolvedLutGrade global) noexcept
{
    if (!settings.enabled) return global;
    if (!ValidateWeatherGradeFeatures(features)) return global;
    if (!ValidateWeatherGradeResponseSettings(settings)) return global;
    return {EvaluateChannel(settings.luminance, features),
        EvaluateChannel(settings.color, features)};
}

bool PublishWeatherGradeCatalog(std::span<const WeatherGradeAggregate> catalog) noexcept
{
    if (!ValidWeatherGradeCatalog(catalog)) return false;
    try
    {
        auto owned = std::make_unique<WeatherGradeCatalog>(catalog.begin(), catalog.end());
        const WeatherGradeCatalog* expected{};
        if (!g_weatherGradeCatalog.compare_exchange_strong(expected, owned.get(),
                std::memory_order_release, std::memory_order_relaxed)) return false;
        (void)owned.release();
        return true;
    }
    catch (...) { return false; }
}

std::span<const WeatherGradeAggregate> ReadWeatherGradeCatalog() noexcept
{
    const auto* catalog = g_weatherGradeCatalog.load(std::memory_order_acquire);
    return catalog ? std::span<const WeatherGradeAggregate>(*catalog) :
        std::span<const WeatherGradeAggregate>{};
}

bool WeatherGradeResponseCache::Matches(std::span<const WeatherGradeAggregate> catalog,
    const WeatherGradeResponseSettings& settings) const noexcept
{
    return ready_ && catalog_.data() == catalog.data() &&
        catalog_.size() == catalog.size() && settings_ == settings;
}

bool WeatherGradeResponseCache::Refresh(std::span<const WeatherGradeAggregate> catalog,
    const WeatherGradeResponseSettings& settings) noexcept
{
    if (Matches(catalog, settings)) return false;
    ready_ = false;
    if (!ValidWeatherGradeCatalog(catalog) ||
        !ValidateWeatherGradeResponseSettings(settings)) return false;
    if (settings.enabled)
        for (std::size_t index = 0; index < catalog.size(); ++index)
            responses_[index] = EvaluateWeatherGradeResponse(settings, catalog[index].features, {});
    catalog_ = catalog;
    settings_ = settings;
    ready_ = true;
    return true;
}

bool WeatherGradeResponseCache::TryResolve(const WeatherGradeAggregate& aggregate,
    ResolvedLutGrade& result) const noexcept
{
    result = {};
    if (!ready_ || !settings_.enabled) return false;
    if (aggregate.cacheIndex == 0 || aggregate.cacheIndex > catalog_.size()) return false;
    const auto index = aggregate.cacheIndex - 1;
    if (aggregate != catalog_[index]) return false;
    result = responses_[index];
    return true;
}

}  // namespace fo4hdr
