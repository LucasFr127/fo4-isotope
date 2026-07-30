// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/PerformanceStatistics.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <limits>

namespace fo4hdr::tests {
namespace {

void TestFixedPerformanceStatistics()
{
    FixedPerformanceStatistics statistics;
    FO4HDR_CHECK(!statistics.Complete());
    FO4HDR_CHECK(statistics.WarmupSamples() == 0);
    FO4HDR_CHECK(statistics.MeasuredSamples() == 0);

    FO4HDR_CHECK(statistics.AddSample(-1.0) ==
        PerformanceSampleResult::rejected);
    FO4HDR_CHECK(statistics.AddSample(
        std::numeric_limits<double>::infinity()) ==
        PerformanceSampleResult::rejected);
    FO4HDR_CHECK(statistics.WarmupSamples() == 0);

    for (std::size_t index = 0;
         index < kPerformanceWarmupSampleCount;
         ++index)
    {
        FO4HDR_CHECK(statistics.AddSample(5000.0) ==
            PerformanceSampleResult::warmingUp);
    }
    FO4HDR_CHECK(statistics.WarmupSamples() ==
        kPerformanceWarmupSampleCount);

    for (std::size_t value = kPerformanceMeasuredSampleCount;
         value > 0;
         --value)
    {
        const auto result = statistics.AddSample(
            static_cast<double>(value));
        const auto expected = value == 1 ?
            PerformanceSampleResult::completed :
            PerformanceSampleResult::measuring;
        FO4HDR_CHECK(result == expected);
    }

    FO4HDR_CHECK(statistics.Complete());
    FO4HDR_CHECK(statistics.MeasuredSamples() ==
        kPerformanceMeasuredSampleCount);
    const auto percentiles = statistics.Percentiles();
    // nearest-rank: ceil(.95 * 1800)=1710, ceil(.99 * 1800)=1782
    FO4HDR_CHECK(percentiles.p95Milliseconds == 1710.0);
    FO4HDR_CHECK(percentiles.p99Milliseconds == 1782.0);
    FO4HDR_CHECK(statistics.AddSample(0.0) ==
        PerformanceSampleResult::alreadyCompleted);
    FO4HDR_CHECK(statistics.Percentiles().p99Milliseconds == 1782.0);

    statistics.Reset();
    FO4HDR_CHECK(!statistics.Complete());
    FO4HDR_CHECK(statistics.WarmupSamples() == 0);
    FO4HDR_CHECK(statistics.MeasuredSamples() == 0);
    FO4HDR_CHECK(statistics.Percentiles().p95Milliseconds == 0.0);
    FO4HDR_CHECK(statistics.Percentiles().p99Milliseconds == 0.0);
}

void TestCpuBudgets()
{
    const auto disabled = GetCpuPerformanceBudget(
        CpuPerformancePath::disabledBoundary);
    FO4HDR_CHECK(!disabled.requiresP95);
    FO4HDR_CHECK(disabled.maximumP99Milliseconds == 0.05);
    FO4HDR_CHECK(PassesCpuPerformanceBudget(
        CpuPerformancePath::disabledBoundary,
        {.p95Milliseconds = 0.049, .p99Milliseconds = 0.05}));
    FO4HDR_CHECK(!PassesCpuPerformanceBudget(
        CpuPerformancePath::disabledBoundary,
        {.p95Milliseconds = 0.049, .p99Milliseconds = 0.051}));

    FO4HDR_CHECK(PassesCpuPerformanceBudget(
        CpuPerformancePath::active,
        {.p95Milliseconds = 0.25, .p99Milliseconds = 0.50}));
    FO4HDR_CHECK(!PassesCpuPerformanceBudget(
        CpuPerformancePath::active,
        {.p95Milliseconds = 0.251, .p99Milliseconds = 0.50}));
    FO4HDR_CHECK(!PassesCpuPerformanceBudget(
        CpuPerformancePath::active,
        {.p95Milliseconds = 0.25, .p99Milliseconds = 0.501}));

    FO4HDR_CHECK(!PassesCpuPerformanceBudget(
        CpuPerformancePath::active,
        {.p95Milliseconds = 0.4, .p99Milliseconds = 0.3}));
    FO4HDR_CHECK(!PassesCpuPerformanceBudget(
        CpuPerformancePath::active,
        {.p95Milliseconds = 0.2,
         .p99Milliseconds =
             std::numeric_limits<double>::quiet_NaN()}));
    FO4HDR_CHECK(!PassesCpuPerformanceBudget(
        static_cast<CpuPerformancePath>(0xFF),
        {.p95Milliseconds = 0.0, .p99Milliseconds = 0.0}));
}

}  // namespace

void RunPerformanceTests()
{
    TestFixedPerformanceStatistics();
    TestCpuBudgets();
}

}  // namespace fo4hdr::tests
