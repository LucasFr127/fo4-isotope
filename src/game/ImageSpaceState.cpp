// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "ImageSpaceObserver.hpp"

#include "FO4Isotope/hooks/HookOwnership.hpp"

#include <bit>
#include <cmath>

namespace fo4hdr {

bool DecodeImageSpaceActiveData(
    std::span<const std::byte> source,
    const std::array<std::size_t, kImageSpaceActiveDataFieldCount>& offsets,
    ImageSpaceActiveData& destination) noexcept
{
    std::array<float, kImageSpaceActiveDataFieldCount> values{};
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto offset = offsets[index];
        if (offset > source.size() ||
            source.size() - offset < sizeof(float))
        {
            return false;
        }

        std::array<std::byte, sizeof(float)> bytes{};
        for (std::size_t byte = 0; byte < bytes.size(); ++byte)
        {
            bytes[byte] = source[offset + byte];
        }
        values[index] = std::bit_cast<float>(bytes);
        if (!std::isfinite(values[index]))
        {
            return false;
        }
    }

    destination = {
        .eyeAdaptSpeed = values[0],
        .bloomBlurRadius = values[1],
        .bloomThreshold = values[2],
        .bloomScale = values[3],
        .receiveBloomThreshold = values[4],
        .white = values[5],
        .sunlightScale = values[6],
        .skyScale = values[7],
        .eyeAdaptStrength = values[8],
        .cinematicSaturation = values[9],
        .cinematicBrightness = values[10],
        .cinematicContrast = values[11]
    };
    return true;
}

bool ValidateImageSpaceEffectListPrefix(
    const ImageSpaceEffectListState& state,
    std::uint16_t requiredPrefixCount) noexcept
{
    return state.capacity >= requiredPrefixCount &&
           state.freeIndex >= requiredPrefixCount &&
           state.size <= state.freeIndex &&
           state.freeIndex <= state.capacity &&
           state.storage != 0;
}

}  // namespace fo4hdr

namespace fo4hdr::game::imagespace_detail {
namespace {
double CodeBrightness(const std::array<std::uint8_t, 3>& color) noexcept
{
    return (2125U * color[0] + 7154U * color[1] + 721U * color[2]) / 2550000.0;
}

double SunlightWarmth(const std::array<std::uint8_t, 3>& color) noexcept
{
    return (static_cast<double>(color[0]) - color[2]) /
        (color[0] + color[1] + color[2] + 1.0);
}
}

bool DecodeWeatherGradeFeatures(
    const std::array<WeatherGradeSlotColors, 8>& colors,
    const std::array<std::uint8_t, 8>& chronologicalSlots,
    std::size_t slot, WeatherGradeFeatures& result) noexcept
{
    result = {};
    if (slot >= colors.size()) return false;
    std::uint32_t visited{};
    std::size_t position{};
    for (std::size_t index = 0; index < chronologicalSlots.size(); ++index)
    {
        const auto candidate = chronologicalSlots[index];
        if (candidate >= colors.size() || (visited & (1U << candidate))) return false;
        visited |= 1U << candidate;
        if (candidate == slot) position = index;
    }
    const auto previous = chronologicalSlots[(position + colors.size() - 1) % colors.size()];
    const auto next = chronologicalSlots[(position + 1) % colors.size()];
    const auto& current = colors[slot];
    result.sunlight = CodeBrightness(current.sunlight);
    result.glare = CodeBrightness(current.glare);
    for (const auto& direction : current.diffuse) result.ambient += CodeBrightness(direction);
    result.ambient /= current.diffuse.size();
    result.colorChange = SunlightWarmth(current.sunlight) -
        (SunlightWarmth(colors[previous].sunlight) + SunlightWarmth(colors[next].sunlight)) / 2.0;
    return ValidateWeatherGradeFeatures(result);
}

void AccumulateWeatherGradeReference(WeatherGradeAggregate& aggregate,
    const WeatherGradeFeatures* features) noexcept
{
    ++aggregate.referenceCount;
    if (features == nullptr || !ValidateWeatherGradeFeatures(*features)) return;
    ++aggregate.validReferenceCount;
    aggregate.features.sunlight += features->sunlight;
    aggregate.features.glare += features->glare;
    aggregate.features.ambient += features->ambient;
    aggregate.features.colorChange += features->colorChange;
}

void FinalizeWeatherGradeAggregate(WeatherGradeAggregate& aggregate) noexcept
{
    if (aggregate.referenceCount == 0) return;
    aggregate.availability = WeatherGradeAvailability::incomplete;
    if (aggregate.referenceCount != aggregate.validReferenceCount) return;
    const auto divisor = static_cast<double>(aggregate.referenceCount);
    aggregate.features.sunlight /= divisor;
    aggregate.features.glare /= divisor;
    aggregate.features.ambient /= divisor;
    aggregate.features.colorChange /= divisor;
    if (ValidateWeatherGradeFeatures(aggregate.features))
        aggregate.availability = WeatherGradeAvailability::complete;
}

Publication::Token Publication::Begin() noexcept
{
    const auto active = active_.fetch_add(1, std::memory_order_acq_rel);
    const auto revision = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    return {revision, active == 0};
}

void Publication::Invalidate() noexcept
{ revision_.fetch_add(1, std::memory_order_acq_rel); }

bool Publication::Finish(Token& token, const Provenance& candidate) noexcept
{
    if (!token.revision) return false;
    bool published{};
    {
        BoundedAtomicFlagGuard guard(flag_, 1);
        if (guard.OwnsLock() && CanPublish(token)) {
            payload_ = candidate;
            payload_.revision = token.revision;
            published = true;
        }
    }
    // A writer beginning during the copy revokes this payload through the
    // revision/active gates; the flag is never held across native work.
    token = {};
    active_.fetch_sub(1, std::memory_order_acq_rel);
    return published;
}

bool Publication::TryRead(Provenance& result) noexcept
{
    result = {};
    if (active_.load(std::memory_order_acquire)) {
        result.status = Status::updating;
        return false;
    }
    const auto revision = revision_.load(std::memory_order_acquire);
    BoundedAtomicFlagGuard guard(flag_, 1);
    if (!guard.OwnsLock()) {
        result.status = Status::updating;
        return false;
    }
    result = payload_;
    if (!IsCurrent(revision) || result.revision != revision) {
        result = {};
        if (active_.load(std::memory_order_acquire)) result.status = Status::updating;
        return false;
    }
    return true;
}

bool Publication::IsCurrent(std::uint64_t revision) const noexcept
{
    return revision && !active_.load(std::memory_order_acquire) &&
        revision_.load(std::memory_order_acquire) == revision;
}

bool Publication::CanPublish(const Token& token) const noexcept
{
    return token.exclusive && active_.load(std::memory_order_acquire) == 1 &&
        revision_.load(std::memory_order_acquire) == token.revision;
}
}  // namespace fo4hdr::game::imagespace_detail
