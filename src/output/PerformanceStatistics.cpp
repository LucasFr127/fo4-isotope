// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/PerformanceStatistics.hpp"

#include <algorithm>
#include <cmath>

namespace fo4hdr {
namespace {

[[nodiscard]] constexpr std::size_t NearestRankIndex(
    std::size_t count,
    std::size_t numerator,
    std::size_t denominator) noexcept
{
    return ((count * numerator + denominator - 1) / denominator) - 1;
}

[[nodiscard]] bool ValidPercentiles(PerformancePercentiles percentiles) noexcept
{
    if (!std::isfinite(percentiles.p95Milliseconds)) return false;
    if (!std::isfinite(percentiles.p99Milliseconds)) return false;
    return percentiles.p95Milliseconds >= 0.0 &&
        percentiles.p99Milliseconds >= 0.0 &&
        percentiles.p95Milliseconds <= percentiles.p99Milliseconds;
}

}  // namespace

PerformanceSampleResult FixedPerformanceStatistics::AddSample(
    double milliseconds) noexcept
{
    if (complete_) return PerformanceSampleResult::alreadyCompleted;
    if (!std::isfinite(milliseconds) || milliseconds < 0.0)
        return PerformanceSampleResult::rejected;

    if (warmupSamples_ < kPerformanceWarmupSampleCount)
    {
        ++warmupSamples_;
        return PerformanceSampleResult::warmingUp;
    }

    samples_[measuredSamples_++] = milliseconds;
    if (measuredSamples_ < kPerformanceMeasuredSampleCount)
        return PerformanceSampleResult::measuring;

    std::sort(samples_.begin(), samples_.end());
    percentiles_.p95Milliseconds = samples_[NearestRankIndex(
        samples_.size(), 95, 100)];
    percentiles_.p99Milliseconds = samples_[NearestRankIndex(
        samples_.size(), 99, 100)];
    complete_ = true;
    return PerformanceSampleResult::completed;
}

void FixedPerformanceStatistics::Reset() noexcept
{
    warmupSamples_ = 0;
    measuredSamples_ = 0;
    percentiles_ = {};
    complete_ = false;
}

bool PassesFrozenM6PerformanceBudget(
    FrozenM6PerformancePath path,
    PerformancePercentiles percentiles) noexcept
{
    if (!ValidPercentiles(percentiles)) return false;

    const auto budget = GetFrozenM6PerformanceBudget(path);
    return budget.maximumP99Milliseconds > 0.0 &&
        (!budget.requiresP95 ||
            percentiles.p95Milliseconds <= budget.maximumP95Milliseconds) &&
        percentiles.p99Milliseconds <= budget.maximumP99Milliseconds;
}

}  // namespace fo4hdr
