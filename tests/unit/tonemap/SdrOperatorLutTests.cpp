// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "reference/OperatorLutTestSupport.hpp"

#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

namespace fo4hdr::tests {
namespace {

constexpr std::array kSdrAlgorithms{ProductionToneMapperAlgorithm::aces2,
    ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7};

[[nodiscard]] bool IsContained(LinearRgb value)
{
    return std::ranges::all_of(std::array{value.red, value.green, value.blue},
        [](double channel) { return std::isfinite(channel) && channel >= 0.0 && channel <= 1.0; });
}

[[nodiscard]] double Bt709Luminance(LinearRgb value)
{
    return 0.2126 * value.red + 0.7152 * value.green + 0.0722 * value.blue;
}

[[nodiscard]] bool ApproxRgb(LinearRgb actual, LinearRgb expected, double tolerance)
{
    return test::Approx(actual.red, expected.red, tolerance) &&
        test::Approx(actual.green, expected.green, tolerance) &&
        test::Approx(actual.blue, expected.blue, tolerance);
}

void CheckSdrContainment(ProductionToneMapperAlgorithm algorithm)
{
    // GT7's BT.2020 round trip may move an in-range neutral by rounding only.
    FO4HDR_CHECK(ApproxRgb(ContainSdrOperatorOutput(algorithm, {0.18, 0.18, 0.18}),
        {0.18, 0.18, 0.18}, 1.0e-12));
    for (const auto& value : {LinearRgb{-0.2, 0.5, 0.3}, LinearRgb{1.4, 0.2, 0.1},
             LinearRgb{2.0, 2.0, 2.0}, LinearRgb{-1.0, -1.0, -1.0}})
        FO4HDR_CHECK(IsContained(ContainSdrOperatorOutput(algorithm, value)));
    FO4HDR_CHECK(ApproxRgb(ContainSdrOperatorOutput(
        algorithm, {std::numeric_limits<double>::quiet_NaN(), 0.5, 0.5}), {}, 0.0));
}

// ACES2 clamps each channel like its preset. AgX lifts a negative channel to
// zero and returns to the color's BT.709 luminance, so the other channels keep
// their ratio instead of contracting toward that luminance; luminance at or
// above white becomes white.
void CheckAces2AndAgxContainment()
{
    FO4HDR_CHECK(ApproxRgb(ContainSdrOperatorOutput(ProductionToneMapperAlgorithm::aces2,
        {-0.2, 0.5, 1.4}), {0.0, 0.5, 1.0}, 0.0));
    const LinearRgb negative{-0.01, 0.01, 0.17};
    const auto lifted = ContainSdrOperatorOutput(ProductionToneMapperAlgorithm::agX, negative);
    FO4HDR_CHECK(IsContained(lifted) && lifted.red == 0.0);
    FO4HDR_CHECK(test::Approx(Bt709Luminance(lifted), Bt709Luminance(negative), 1.0e-12));
    FO4HDR_CHECK(test::Approx(lifted.green * negative.blue, negative.green * lifted.blue,
        1.0e-12));
    FO4HDR_CHECK(ApproxRgb(ContainSdrOperatorOutput(ProductionToneMapperAlgorithm::agX,
        {1.3, 1.0, 0.6}), {1.0, 1.0, 1.0}, 0.0));
}

// A color that still exceeds BT.709 white, after GT7's BT.2020 clip or as AgX's
// output, keeps its luminance and chroma direction while chroma contracts into
// range, instead of losing a channel. Colors well inside BT.709 are unchanged.
void CheckLuminanceFit(ProductionToneMapperAlgorithm algorithm, LinearRgb input,
    LinearRgb overWhite)
{
    const auto fitted = ContainSdrOperatorOutput(algorithm, input);
    FO4HDR_CHECK(IsContained(fitted));
    FO4HDR_CHECK(overWhite.red > 1.0);
    const auto luminance = Bt709Luminance(overWhite);
    FO4HDR_CHECK(test::Approx(Bt709Luminance(fitted), luminance, 1.0e-12));
    const auto scale = (fitted.red - luminance) / (overWhite.red - luminance);
    FO4HDR_CHECK(scale > 0.0 && scale < 1.0);
    const auto toward = [=](double channel) { return luminance + scale * (channel - luminance); };
    FO4HDR_CHECK(ApproxRgb(fitted,
        {toward(overWhite.red), toward(overWhite.green), toward(overWhite.blue)}, 1.0e-12));
    FO4HDR_CHECK(ApproxRgb(ContainSdrOperatorOutput(algorithm, {0.5, 0.3, 0.2}),
        {0.5, 0.3, 0.2}, 1.0e-12));
}

void TestSdrContainment()
{
    for (const auto algorithm : kSdrAlgorithms)
        CheckSdrContainment(algorithm);
    CheckAces2AndAgxContainment();
    // GT7 first clips each BT.2020 channel at white like its official sample.
    CheckLuminanceFit(ProductionToneMapperAlgorithm::gt7, ConvertBt2020ToBt709({1.3, 0.4, 0.2}),
        ConvertBt2020ToBt709({1.0, 0.4, 0.2}));
    CheckLuminanceFit(ProductionToneMapperAlgorithm::agX, {1.4, 0.6, 0.5}, {1.4, 0.6, 0.5});
}

// Tint, adjacent jitter and running drawdown in 10-bit PQ codes at the metric
// mapping, against the HDR CPU bounds. SDR neutrals stay neutral in BT.709.
void CheckSdrNeutral(const OperatorLutData& lut)
{
    constexpr std::size_t sampleCount{4096};
    const auto maximumEv = std::log2(kOperatorLutSceneMaximum);
    std::array<double, 3> previous{};
    std::array<double, 3> running{};
    double tint{};
    double jitter{};
    double drawdown{};
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        const auto fraction = static_cast<double>(index) / static_cast<double>(sampleCount - 1U);
        const auto value = static_cast<double>(static_cast<float>(
            std::exp2(-24.0 + (maximumEv + 24.0) * fraction)));
        const auto sampled = test::OperatorLutMetricNits(lut,
            test::SampleOperatorLut(lut, {value, value, value}));
        const std::array codes{1023.0 * EncodeSt2084Pq(sampled.red),
            1023.0 * EncodeSt2084Pq(sampled.green), 1023.0 * EncodeSt2084Pq(sampled.blue)};
        tint = std::max({tint, std::abs(codes[0] - codes[1]), std::abs(codes[1] - codes[2]),
            std::abs(codes[0] - codes[2])});
        for (std::size_t channel = 0; channel < codes.size(); ++channel)
        {
            if (index != 0U) jitter = std::max(jitter, previous[channel] - codes[channel]);
            previous[channel] = codes[channel];
            running[channel] = std::max(running[channel], codes[channel]);
            drawdown = std::max(drawdown, running[channel] - codes[channel]);
        }
    }
    std::fprintf(stdout, "neutral SDR LUT CPU algorithm=%u samples=%zu "
        "tint_max_pq_codes=%.9f/1.25 adjacent_jitter_max_pq_codes=%.9f/0.50 "
        "running_max_drawdown_pq_codes=%.9f/1.75\n",
        static_cast<unsigned>(lut.algorithm), sampleCount, tint, jitter, drawdown);
    FO4HDR_CHECK(tint <= 1.25);
    FO4HDR_CHECK(jitter <= 0.5);
    FO4HDR_CHECK(drawdown <= 1.75);
}

void CheckSdrOperatorLut(ProductionToneMapperAlgorithm algorithm)
{
    const auto lut = BuildSdrOperatorLut(algorithm);
    FO4HDR_CHECK(lut.IsValid());
    if (!lut.IsValid()) return;
    FO4HDR_CHECK(lut.target == OperatorLutTarget::sdr);
    FO4HDR_CHECK(lut.displayPeakNits == SdrOperatorWhiteNits(algorithm));
    FO4HDR_CHECK(lut.edge == OperatorLutEdge(algorithm));
    const auto shaper = OperatorLutShaperParameters(algorithm, lut.displayPeakNits);
    FO4HDR_CHECK(std::isfinite(shaper[0]) && shaper[0] > 0.0F && shaper[0] < 1.0F);
    // An SDR table is never valid as an HDR table or at another white.
    auto relabeled = lut;
    relabeled.target = OperatorLutTarget::hdr10;
    FO4HDR_CHECK(!relabeled.IsValid());
    relabeled = lut;
    relabeled.displayPeakNits = 1000.0;
    FO4HDR_CHECK(!relabeled.IsValid());

    const auto black = test::SampleOperatorLut(lut, {});
    FO4HDR_CHECK(black.red == 0.0 && black.green == 0.0 && black.blue == 0.0);
    bool contained{true};
    for (const auto scene : test::BuildOperatorLutCorpus(lut.displayPeakNits))
        contained = contained && IsContained(test::SampleOperatorLut(lut, scene));
    FO4HDR_CHECK(contained);

    const auto quality = test::EvaluateOperatorLutQuality(lut);
    std::fprintf(stdout, "SDR operator LUT CPU quality algorithm=%u white=%.0f samples=%u "
        "max_delta_e_itp=%.9f broad_p99_delta_e_itp=%.9f holdout_p99_delta_e_itp=%.9f\n",
        static_cast<unsigned>(algorithm), lut.displayPeakNits, quality.sampleCount,
        quality.maximumDeltaEItp, quality.broadP99DeltaEItp, quality.holdoutP99DeltaEItp);
    FO4HDR_CHECK(quality.MeetsPracticalLimit(algorithm));
    CheckSdrNeutral(lut);
}

void TestSdrBuildRequests()
{
    FO4HDR_CHECK(SdrOperatorWhiteNits(ProductionToneMapperAlgorithm::native) == 0.0);
    FO4HDR_CHECK(!BuildSdrOperatorLut(ProductionToneMapperAlgorithm::native).IsValid());
    FO4HDR_CHECK(!BuildSdrOperatorLut(ProductionToneMapperAlgorithm::agX,
        {.agxContrast = kMaximumAgxContrast + 0.1}).IsValid());
    FO4HDR_CHECK(!BuildSdrOperatorLut(ProductionToneMapperAlgorithm::agX,
        {.agxMiddleGrayEv = 0.1}).IsValid());
}

}  // namespace

void RunSdrOperatorLutTests()
{
    TestSdrContainment();
    TestSdrBuildRequests();
    for (const auto algorithm : kSdrAlgorithms)
        CheckSdrOperatorLut(algorithm);
}

}  // namespace fo4hdr::tests
