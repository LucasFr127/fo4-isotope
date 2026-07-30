// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

inline constexpr std::size_t kPerformanceWarmupSampleCount{300};
inline constexpr std::size_t kPerformanceMeasuredSampleCount{1800};

enum class PerformanceSampleResult : std::uint8_t
{
    rejected,
    warmingUp,
    measuring,
    completed,
    alreadyCompleted
};

struct PerformancePercentiles
{
    double p95Milliseconds{};
    double p99Milliseconds{};
};

// Fixed-storage, single-window statistics. Percentiles use the nearest-rank
// definition: rank=ceil(p*N),
// with the one-based rank converted to a zero-based sorted-array index.
class FixedPerformanceStatistics
{
public:
    [[nodiscard]] PerformanceSampleResult AddSample(
        double milliseconds) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool Complete() const noexcept { return complete_; }
    [[nodiscard]] std::size_t WarmupSamples() const noexcept
    {
        return warmupSamples_;
    }
    [[nodiscard]] std::size_t MeasuredSamples() const noexcept
    {
        return measuredSamples_;
    }
    [[nodiscard]] PerformancePercentiles Percentiles() const noexcept
    {
        return percentiles_;
    }

private:
    std::array<double, kPerformanceMeasuredSampleCount> samples_{};
    std::size_t warmupSamples_{};
    std::size_t measuredSamples_{};
    PerformancePercentiles percentiles_{};
    bool complete_{};
};

enum class CpuPerformancePath : std::uint8_t
{
    disabledBoundary,
    active
};

struct CpuPerformanceBudget
{
    double maximumP95Milliseconds{};
    double maximumP99Milliseconds{};
    bool requiresP95{};
};

[[nodiscard]] constexpr CpuPerformanceBudget GetCpuPerformanceBudget(
    CpuPerformancePath path) noexcept
{
    switch (path)
    {
    case CpuPerformancePath::disabledBoundary:
        return {.maximumP99Milliseconds = 0.05, .requiresP95 = false};
    case CpuPerformancePath::active:
        return {
            .maximumP95Milliseconds = 0.25,
            .maximumP99Milliseconds = 0.50,
            .requiresP95 = true
        };
    }
    return {};
}

[[nodiscard]] bool PassesCpuPerformanceBudget(
    CpuPerformancePath path,
    PerformancePercentiles percentiles) noexcept;

}  // namespace fo4hdr
