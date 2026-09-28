// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/output/PerformanceStatistics.hpp"
#include "TestHarness.hpp"

#include <limits>
#include <string_view>

namespace fo4hdr::tests {
namespace {

constexpr std::string_view kBaseConfiguration = R"ini(
[General]
DeveloperMode=0

[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";

void TestOptionalPerformanceConfiguration()
{
    auto parsed = ParsePluginConfiguration(kBaseConfiguration);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.measurePerformance);

    constexpr std::string_view enabled = R"ini(
[General]
DeveloperMode=1

[Debug]
TraceRenderPipeline=0
MeasurePerformance=1
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(enabled);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.measurePerformance);

    constexpr std::string_view withoutDeveloperMode = R"ini(
[General]
DeveloperMode=0

[Debug]
TraceRenderPipeline=0
MeasurePerformance=1
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(withoutDeveloperMode);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::invalidCombination);

    constexpr std::string_view duplicate = R"ini(
[General]
DeveloperMode=1

[Debug]
TraceRenderPipeline=0
MeasurePerformance=0
MeasurePerformance=1
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(duplicate);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::duplicateKey);
}

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

void TestFrozenM6Budgets()
{
    const auto disabled = GetFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::disabledBoundaryCpu);
    FO4HDR_CHECK(!disabled.requiresP95);
    FO4HDR_CHECK(disabled.maximumP99Milliseconds == 0.05);
    FO4HDR_CHECK(PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::disabledBoundaryCpu,
        {.p95Milliseconds = 0.049, .p99Milliseconds = 0.05}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::disabledBoundaryCpu,
        {.p95Milliseconds = 0.049, .p99Milliseconds = 0.051}));

    FO4HDR_CHECK(PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeCpu,
        {.p95Milliseconds = 0.25, .p99Milliseconds = 0.50}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeCpu,
        {.p95Milliseconds = 0.251, .p99Milliseconds = 0.50}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeCpu,
        {.p95Milliseconds = 0.25, .p99Milliseconds = 0.501}));

    FO4HDR_CHECK(PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeGpu,
        {.p95Milliseconds = 1.0, .p99Milliseconds = 1.5}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeGpu,
        {.p95Milliseconds = 1.01, .p99Milliseconds = 1.5}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeGpu,
        {.p95Milliseconds = 1.0, .p99Milliseconds = 1.51}));

    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeCpu,
        {.p95Milliseconds = 0.4, .p99Milliseconds = 0.3}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        FrozenM6PerformancePath::activeCpu,
        {.p95Milliseconds = 0.2,
         .p99Milliseconds =
             std::numeric_limits<double>::quiet_NaN()}));
    FO4HDR_CHECK(!PassesFrozenM6PerformanceBudget(
        static_cast<FrozenM6PerformancePath>(0xFF),
        {.p95Milliseconds = 0.0, .p99Milliseconds = 0.0}));
}

}  // namespace

void RunPerformanceTests()
{
    TestOptionalPerformanceConfiguration();
    TestFixedPerformanceStatistics();
    TestFrozenM6Budgets();
}

}  // namespace fo4hdr::tests
