// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fo4hdr {

inline constexpr double kMaximumWeatherGradeGain{2.0};
inline constexpr double kMaximumWeatherGradeCoefficient{1000.0};
inline constexpr std::size_t kMaximumWeatherGradeImageSpaces{8192};

struct WeatherGradeFeatures
{
    double sunlight{};
    double glare{};
    double ambient{};
    double colorChange{};
    bool operator==(const WeatherGradeFeatures&) const = default;
};

enum class WeatherGradeAvailability : std::uint8_t
{
    unavailable,
    complete,
    incomplete
};

struct WeatherGradeAggregate
{
    WeatherGradeFeatures features{};
    std::uint32_t referenceCount{};
    std::uint32_t validReferenceCount{};
    WeatherGradeAvailability availability{WeatherGradeAvailability::unavailable};
    // Zero is unavailable; positive indices address the immutable plain catalog.
    std::uint32_t cacheIndex{};
    bool operator==(const WeatherGradeAggregate&) const = default;
};

struct WeatherGradeChannel
{
    double base{100.0};
    double gain{1.0};
    double minimum{};
    double maximum{100.0};
    double intercept{};
    std::array<double, 4> coefficients{};
    bool operator==(const WeatherGradeChannel&) const = default;
};

struct WeatherGradeResponseSettings
{
    bool enabled{true};
    WeatherGradeChannel luminance{100.0, 1.0, 0.0, 100.0,
        105.853889,
        {-21.104094, -82.698734, 80.839967, -66.328886}};
    WeatherGradeChannel color{100.0, 1.0, 0.0, 100.0,
        118.057618,
        {-19.199377, -57.753864, 11.932621, -84.208685}};
    bool operator==(const WeatherGradeResponseSettings&) const = default;
};

struct ResolvedLutGrade
{
    double luminancePercent{100.0};
    double colorPercent{100.0};
    bool operator==(const ResolvedLutGrade&) const = default;
};

[[nodiscard]] bool ValidateWeatherGradeFeatures(
    const WeatherGradeFeatures& features) noexcept;
[[nodiscard]] bool ValidateWeatherGradeChannel(
    const WeatherGradeChannel& channel) noexcept;
[[nodiscard]] bool ValidateWeatherGradeResponseSettings(
    const WeatherGradeResponseSettings& settings) noexcept;
[[nodiscard]] ResolvedLutGrade EvaluateWeatherGradeResponse(
    const WeatherGradeResponseSettings& settings,
    const WeatherGradeFeatures& features, ResolvedLutGrade fallback) noexcept;

// Startup publication copies plain aggregates and retains them until process exit.
[[nodiscard]] bool PublishWeatherGradeCatalog(
    std::span<const WeatherGradeAggregate> catalog) noexcept;
[[nodiscard]] std::span<const WeatherGradeAggregate> ReadWeatherGradeCatalog() noexcept;

class WeatherGradeResponseCache final
{
public:
    // Single-thread owner; catalog storage must remain immutable and alive.
    [[nodiscard]] bool Refresh(std::span<const WeatherGradeAggregate> catalog,
        const WeatherGradeResponseSettings& settings) noexcept;
    [[nodiscard]] bool Matches(std::span<const WeatherGradeAggregate> catalog,
        const WeatherGradeResponseSettings& settings) const noexcept;
    [[nodiscard]] bool TryResolve(const WeatherGradeAggregate& aggregate,
        ResolvedLutGrade& result) const noexcept;

private:
    std::span<const WeatherGradeAggregate> catalog_{};
    WeatherGradeResponseSettings settings_{};
    std::array<ResolvedLutGrade, kMaximumWeatherGradeImageSpaces> responses_{};
    bool ready_{};
};

// Keeps the per-slot response of the last verified draw for a draw whose checks
// return no answer. The response depends only on which LUT sits in which slot,
// so it stays right while the same four views are bound.
// Single-thread owner.
class WeatherGradeResponseHold final
{
public:
    using Frame = std::uint64_t;
    using Grades = std::array<ResolvedLutGrade, 4>;
    // Identities of the views bound to the four LUT slots; compared, never dereferenced.
    using Views = std::array<std::uint64_t, 4>;
    // After a longer gap between draws an identity may name a different view.
    static constexpr Frame kGapFrames = 3;

    // Automatic is the mask of slots whose grade came from the weather response.
    void Store(Frame frame, const Views& views, const Grades& grades,
        std::uint32_t automatic) noexcept;
    void Reset() noexcept;
    // A held draw renews the hold, so it lasts while the same views stay bound.
    [[nodiscard]] bool TryHold(Frame frame, const Views& views, Grades& grades,
        std::uint32_t& automatic) noexcept;

private:
    Grades grades_{};
    Views views_{};
    Frame frame_{};
    std::uint32_t automatic_{};
    bool stored_{};
};

}  // namespace fo4hdr
