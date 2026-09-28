// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "reference/OperatorLutTestSupport.hpp"
#include "FO4Isotope/tonemap/Aces2.hpp"
#include "FO4Isotope/tonemap/Agx.hpp"
#include "FO4Isotope/tonemap/Gt7.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>

namespace fo4hdr::tests {
namespace {

void CheckRgb(LinearRgb actual, LinearRgb expected, double tolerance)
{
    FO4HDR_CHECK(test::Approx(actual.red, expected.red, tolerance));
    FO4HDR_CHECK(test::Approx(actual.green, expected.green, tolerance));
    FO4HDR_CHECK(test::Approx(actual.blue, expected.blue, tolerance));
}

using test::HalfToDouble;
using test::SampleOperatorLut;

[[nodiscard]] LinearRgb MapAnalytic(
    ProductionToneMapperAlgorithm algorithm,
    LinearRgb scene,
    double peak,
    const Aces2Transform* aces2) noexcept
{
    if (algorithm == ProductionToneMapperAlgorithm::aces2Lmt)
        return aces2 != nullptr ?
            MapAces2LmtToBt2020Nits(*aces2, scene) : LinearRgb{};
    if (algorithm == ProductionToneMapperAlgorithm::agX)
        return MapAgxToBt2020Nits(scene, peak);
    if (algorithm == ProductionToneMapperAlgorithm::gt7)
        return MapGt7ToBt2020Nits(scene, peak);
    return {};
}

constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
constexpr auto infinity = std::numeric_limits<double>::infinity();
constexpr auto invalidAlgorithm =
    static_cast<ProductionToneMapperAlgorithm>(0xFF);
constexpr std::array algorithms{
    ProductionToneMapperAlgorithm::aces2Lmt,
    ProductionToneMapperAlgorithm::agX,
    ProductionToneMapperAlgorithm::gt7};

void TestInterpolationFixture()
{
    // A small padded coefficient texture isolates interpolation from operator
    // math. Each slice is constant in x/y; the reconstructed 1500-nit blue is
    // contained only after the coefficient lattice has been evaluated.
    OperatorLutData interpolationFixture;
    interpolationFixture.edge = 4U;
    interpolationFixture.textureEdge =
        interpolationFixture.edge + kOperatorLutCoefficientPadding;
    interpolationFixture.displayPeakNits = 1000.0;
    const auto physicalEdge = interpolationFixture.textureEdge;
    interpolationFixture.rgba16.resize(
        static_cast<std::size_t>(physicalEdge) * physicalEdge *
        physicalEdge * 4U);
    constexpr std::array<std::uint16_t, 6> fixtureRed{
        0x0000U, 0x4900U, 0x4D00U, 0x4F80U, 0x4D00U, 0x4900U};
    for (std::uint32_t z = 0U; z < physicalEdge; ++z)
        for (std::uint32_t texel = 0U;
             texel < physicalEdge * physicalEdge; ++texel)
        {
            const auto index = (z * physicalEdge * physicalEdge + texel) * 4U;
            interpolationFixture.rgba16[index] = fixtureRed[z];
            interpolationFixture.rgba16[index + 1U] = 0x0000U;
            interpolationFixture.rgba16[index + 2U] = 0x65DCU; // 1500
            interpolationFixture.rgba16[index + 3U] = 0x3C00U;
        }
    constexpr LinearRgb reconstructed{1180.0 / 48.0, 0.0, 1500.0};
    FO4HDR_CHECK(reconstructed.blue > interpolationFixture.displayPeakNits);
    for (const auto algorithm : algorithms)
    {
        interpolationFixture.algorithm = algorithm;
        const auto sceneBlue = DecodeOperatorLutCoordinate(
            0.5, algorithm, interpolationFixture.displayPeakNits);
        const auto sampled = SampleOperatorLut(
            interpolationFixture, {0.0, 0.0, sceneBlue});
        const auto contained = algorithm == ProductionToneMapperAlgorithm::agX ?
            ApplyAgxDisplayGamut(reconstructed, 1000.0) :
            ContainBt2020Nits(reconstructed, 1000.0);
        CheckRgb(sampled, contained, 1.0e-9);
        for (const auto channel : {sampled.red, sampled.green, sampled.blue})
            FO4HDR_CHECK(std::isfinite(channel) && channel >= 0.0 &&
                channel <= interpolationFixture.displayPeakNits);
    }
}

void TestInvalidOperatorLuts()
{
    FO4HDR_CHECK(!OperatorLutData{}.IsValid());
    FO4HDR_CHECK(!BuildOperatorLut(invalidAlgorithm, 1000.0).IsValid());
    FO4HDR_CHECK(OperatorLutInputShaperMinimum(
        invalidAlgorithm, 1000.0) == 0.0);
    FO4HDR_CHECK(OperatorLutInputShaperMaximum(
        invalidAlgorithm, 1000.0) == 0.0);
    FO4HDR_CHECK(DecodeOperatorLutCoordinate(
        0.5, invalidAlgorithm, 1000.0) == 0.0);
    FO4HDR_CHECK(EncodeOperatorLutCoordinate(
        1.0, invalidAlgorithm, 1000.0) == 0.0);

    for (const auto algorithm : algorithms)
    {
        for (const auto invalidPeak :
             {nan, infinity, -infinity, -1.0, 0.0, 399.0, 2001.0})
        {
            FO4HDR_CHECK(!BuildOperatorLut(algorithm, invalidPeak).IsValid());
            FO4HDR_CHECK(OperatorLutInputShaperMinimum(
                algorithm, invalidPeak) == 0.0);
            FO4HDR_CHECK(OperatorLutInputShaperMaximum(
                algorithm, invalidPeak) == 0.0);
            FO4HDR_CHECK(EncodeOperatorLutCoordinate(
                1.0, algorithm, invalidPeak) == 0.0);
            FO4HDR_CHECK(DecodeOperatorLutCoordinate(
                0.5, algorithm, invalidPeak) == 0.0);
        }
    }
}

void CheckShaperContract(ProductionToneMapperAlgorithm algorithm, double peak)
{
    constexpr auto maximumInput = 65504.0;
    FO4HDR_CHECK(OperatorLutInputShaperMinimum(algorithm, peak) <
        OperatorLutInputShaperMaximum(algorithm, peak));
    for (const auto invalidCoordinate : {nan, infinity, -infinity})
        FO4HDR_CHECK(DecodeOperatorLutCoordinate(
            invalidCoordinate, algorithm, peak) == 0.0);
    FO4HDR_CHECK(DecodeOperatorLutCoordinate(
        -1.0, algorithm, peak) == 0.0);
    FO4HDR_CHECK(DecodeOperatorLutCoordinate(
        0.0, algorithm, peak) == 0.0);
    FO4HDR_CHECK(std::abs(DecodeOperatorLutCoordinate(
        1.0, algorithm, peak) - maximumInput) <= 1.0e-8);
    FO4HDR_CHECK(std::abs(DecodeOperatorLutCoordinate(
        2.0, algorithm, peak) - maximumInput) <= 1.0e-8);
    FO4HDR_CHECK(EncodeOperatorLutCoordinate(
        65504.0, algorithm, peak) == 1.0);
    FO4HDR_CHECK(EncodeOperatorLutCoordinate(
        131008.0, algorithm, peak) == 1.0);
    const auto shaper = OperatorLutShaperParameters(algorithm, peak);
    FO4HDR_CHECK(std::isfinite(shaper[0]) && shaper[0] > 0.0F && shaper[0] < 1.0F);
    FO4HDR_CHECK(std::isfinite(shaper[1]) && shaper[1] > 0.0F);

    double previous{-1.0};
    double previousDarkCoordinate = -1.0;
    for (unsigned step = 0; step <= 1024; ++step)
    {
        const auto value = 0.0625 * step / 1024.0;
        const auto coordinate = EncodeOperatorLutCoordinate(value, algorithm, peak);
        FO4HDR_CHECK(coordinate > previousDarkCoordinate);
        FO4HDR_CHECK(std::abs(DecodeOperatorLutCoordinate(coordinate, algorithm, peak) -
            value) < 1.0e-10);
        previousDarkCoordinate = coordinate;
    }
    for (const auto value : {0.0, 0.000001, 0.001, 0.18, 1.0,
             16.0, 20.0, 32.0, 64.0, 256.0, 4096.0, 65504.0})
    {
        const auto encoded = EncodeOperatorLutCoordinate(
            value, algorithm, peak);
        FO4HDR_CHECK(encoded >= previous);
        if (value < maximumInput) FO4HDR_CHECK(encoded > previous);
        previous = encoded;
        const auto decoded = DecodeOperatorLutCoordinate(
            encoded, algorithm, peak);
        FO4HDR_CHECK(std::abs(decoded - std::min(value, maximumInput)) <=
            1.0e-10 * std::max(value, 1.0));
    }
}

[[nodiscard]] bool HasNegativeChannel(LinearRgb value)
{
    return value.red < 0.0 || value.green < 0.0 || value.blue < 0.0;
}

struct CorpusStatistics
{
    double peak;
    double maximumError{};
    LinearRgb maximumErrorScene{};
    std::size_t maximumErrorChannel{};
    double broadTotalError{};
    double holdoutTotalError{};
    bool outsideBt709Observed{};
    bool finiteAndContained{true};

    void Record(LinearRgb scene, LinearRgb sampled, LinearRgb analytic, bool broadSample)
    {
        const auto bt709 = ConvertBt2020ToBt709ForComparison(sampled);
        outsideBt709Observed = outsideBt709Observed || HasNegativeChannel(bt709);
        const std::array expected{
            analytic.red, analytic.green, analytic.blue};
        const std::array actual{
            sampled.red, sampled.green, sampled.blue};
        for (std::size_t channel = 0; channel < 3U; ++channel)
        {
            finiteAndContained = finiteAndContained &&
                std::isfinite(actual[channel]) &&
                actual[channel] >= 0.0 && actual[channel] <= peak;
            const auto error = std::abs(
                actual[channel] - expected[channel]);
            if (error > maximumError)
            {
                maximumError = error;
                maximumErrorScene = scene;
                maximumErrorChannel = channel;
            }
            if (broadSample)
                broadTotalError += error;
            else
                holdoutTotalError += error;
        }
    }

    void Report(ProductionToneMapperAlgorithm algorithm,
        std::size_t broadCount, std::size_t holdoutCount) const
    {
        // The LMT's high-curvature response needs a narrowly scoped
        // 0.7%-of-peak representation bound. Every other operator keeps
        // the established 0.5% gate; all modes retain the same mean gate.
        const auto maximumBound = std::max(1.0, peak *
            (algorithm == ProductionToneMapperAlgorithm::aces2Lmt ?
                0.007 : 0.005));
        const auto meanBound = std::max(0.1, peak * 0.0005);
        const auto broadMeanError = broadTotalError /
            static_cast<double>(broadCount * 3U);
        const auto holdoutMeanError = holdoutTotalError /
            static_cast<double>(holdoutCount * 3U);
        std::fprintf(stdout,
            "operator LUT CPU %s: algorithm=%u peak=%.0f "
            "broadSamples=%zu holdoutSamples=%zu "
            "allMax=%.9f/%.9f broadMean=%.9f/%.9f "
            "holdoutMean=%.9f/%.9f "
            "scene=(%.6f,%.6f,%.6f) channel=%zu\n",
            "parity",
            static_cast<unsigned>(algorithm), peak, broadCount,
            holdoutCount, maximumError, maximumBound,
            broadMeanError, meanBound, holdoutMeanError, meanBound,
            maximumErrorScene.red, maximumErrorScene.green,
            maximumErrorScene.blue, maximumErrorChannel);
        FO4HDR_CHECK(maximumError <= maximumBound);
        FO4HDR_CHECK(broadMeanError <= meanBound);
        FO4HDR_CHECK(holdoutMeanError <= meanBound);
        FO4HDR_CHECK(outsideBt709Observed);
        FO4HDR_CHECK(finiteAndContained);
    }
};

struct NeutralStatistics
{
    std::array<double, 3> previousNeutralPqCodes{};
    std::array<double, 3> runningNeutralMaximumPqCodes{};
    double maximumNeutralTintPqCodes{};
    double maximumNeutralAdjacentDecreasePqCodes{};
    double maximumNeutralDrawdownPqCodes{};
    double maximumNeutralErrorNits{};

    void Record(LinearRgb reference, LinearRgb sampled, std::size_t sampleIndex)
    {
        maximumNeutralErrorNits = std::max({maximumNeutralErrorNits,
            std::abs(sampled.red - reference.red),
            std::abs(sampled.green - reference.green),
            std::abs(sampled.blue - reference.blue)});
        const std::array referencePqCodes{
            1023.0 * EncodeSt2084Pq(reference.red),
            1023.0 * EncodeSt2084Pq(reference.green),
            1023.0 * EncodeSt2084Pq(reference.blue)};
        const std::array sampledPqCodes{
            1023.0 * EncodeSt2084Pq(sampled.red),
            1023.0 * EncodeSt2084Pq(sampled.green),
            1023.0 * EncodeSt2084Pq(sampled.blue)};
        for (std::size_t first = 0; first < 3U; ++first)
        {
            if (sampleIndex != 0U)
                maximumNeutralAdjacentDecreasePqCodes = std::max(
                    maximumNeutralAdjacentDecreasePqCodes,
                    previousNeutralPqCodes[first] -
                        sampledPqCodes[first]);
            previousNeutralPqCodes[first] = sampledPqCodes[first];
            runningNeutralMaximumPqCodes[first] = std::max(
                runningNeutralMaximumPqCodes[first],
                sampledPqCodes[first]);
            maximumNeutralDrawdownPqCodes = std::max(
                maximumNeutralDrawdownPqCodes,
                runningNeutralMaximumPqCodes[first] -
                    sampledPqCodes[first]);
            for (std::size_t second = first + 1U;
                 second < 3U; ++second)
            {
                const auto sourceDifference =
                    referencePqCodes[first] -
                    referencePqCodes[second];
                const auto sampledDifference =
                    sampledPqCodes[first] - sampledPqCodes[second];
                maximumNeutralTintPqCodes = std::max(
                    maximumNeutralTintPqCodes,
                    std::abs(sampledDifference - sourceDifference));
            }
        }
    }

    void Report(ProductionToneMapperAlgorithm algorithm, double peak, double maximumBound) const
    {
        constexpr std::size_t neutralSampleCount{4096};
        constexpr double neutralTintBoundPqCodes{1.25};
        // Adjacent reconstructed samples may jitter by half a 10-bit
        // code; cumulative ripple has a separate two-endpoint budget.
        constexpr double adjacentJitterBoundPqCodes{0.5};
        constexpr double neutralRippleBoundPqCodes{1.75};
        std::fprintf(stdout,
            "neutral LUT CPU algorithm=%u peak=%.0f samples=%zu "
            "max_nits=%.9f/%.9f "
            "tint_max_pq_codes=%.9f/%.2f "
            "adjacent_jitter_max_pq_codes=%.9f/%.2f "
            "running_max_drawdown_pq_codes=%.9f/%.2f\n",
            static_cast<unsigned>(algorithm), peak, neutralSampleCount,
            maximumNeutralErrorNits, maximumBound,
            maximumNeutralTintPqCodes, neutralTintBoundPqCodes,
            maximumNeutralAdjacentDecreasePqCodes,
            adjacentJitterBoundPqCodes,
            maximumNeutralDrawdownPqCodes,
            neutralRippleBoundPqCodes);
        FO4HDR_CHECK(maximumNeutralTintPqCodes <=
            neutralTintBoundPqCodes);
        FO4HDR_CHECK(maximumNeutralErrorNits <= maximumBound);
        FO4HDR_CHECK(maximumNeutralAdjacentDecreasePqCodes <=
            adjacentJitterBoundPqCodes);
        FO4HDR_CHECK(maximumNeutralDrawdownPqCodes <=
            neutralRippleBoundPqCodes);
    }
};

struct LutTestCase
{
    ProductionToneMapperAlgorithm algorithm;
    double peak;
    OperatorLutData& lut;
    const Aces2Transform* aces2;

    [[nodiscard]] double MaximumErrorBound() const
    {
        return std::max(1.0, peak *
            (algorithm == ProductionToneMapperAlgorithm::aces2Lmt ? 0.007 : 0.005));
    }

    void CheckMetadata() const
    {
        FO4HDR_CHECK(lut.algorithm == algorithm);
        FO4HDR_CHECK(lut.edge == OperatorLutEdge(algorithm));
        FO4HDR_CHECK(lut.textureEdge ==
            lut.edge + kOperatorLutCoefficientPadding);
        FO4HDR_CHECK(lut.inputShaperMinimum ==
            OperatorLutInputShaperMinimum(algorithm, peak));
        FO4HDR_CHECK(lut.inputShaperMaximum ==
            OperatorLutInputShaperMaximum(algorithm, peak));
        FO4HDR_CHECK(lut.inputOffset == OperatorLutInputOffset(algorithm));
        FO4HDR_CHECK(lut.rgba16.size() ==
            static_cast<std::size_t>(lut.textureEdge) *
                lut.textureEdge * lut.textureEdge * 4U);
    }

    void CheckTexels() const
    {
        bool finiteRawTexels{true};
        bool opaqueAlpha{true};
        for (std::size_t index = 0;
             index < lut.rgba16.size(); index += 4U)
        {
            for (std::size_t channel = 0; channel < 3U; ++channel)
                finiteRawTexels = finiteRawTexels && std::isfinite(
                    HalfToDouble(lut.rgba16[index + channel]));
            opaqueAlpha = opaqueAlpha &&
                HalfToDouble(lut.rgba16[index + 3U]) == 1.0;
        }
        FO4HDR_CHECK(finiteRawTexels);
        FO4HDR_CHECK(opaqueAlpha);
    }

    void CheckCorpus() const
    {
        auto corpus = test::BuildOperatorLutCorpus(peak);
        const auto broadCount = corpus.size();
        const auto holdout = test::BuildOperatorLutHoldoutCorpus();
        const auto holdoutCount = holdout.size();
        corpus.insert(corpus.end(), holdout.begin(), holdout.end());
        FO4HDR_CHECK(broadCount != 0U);
        FO4HDR_CHECK(holdoutCount == 32768U);
        CorpusStatistics statistics{peak};
        for (std::size_t sampleIndex = 0;
             sampleIndex < corpus.size(); ++sampleIndex)
        {
            const auto scene = corpus[sampleIndex];
            const auto analytic =
                MapAnalytic(algorithm, scene, peak, aces2);
            const auto sampled = SampleOperatorLut(lut, scene);
            statistics.Record(scene, sampled, analytic, sampleIndex < broadCount);
        }
        statistics.Report(algorithm, broadCount, holdoutCount);
    }

    void CheckNeutral() const
    {
        constexpr double maximumInput = 65504.0;
        const auto maximumBound = MaximumErrorBound();
        constexpr std::size_t neutralSampleCount{4096};
        const auto maximumInputEv = std::log2(maximumInput);
        NeutralStatistics statistics;
        for (std::size_t sampleIndex = 0;
             sampleIndex < neutralSampleCount; ++sampleIndex)
        {
            const auto fraction = static_cast<double>(sampleIndex) /
                static_cast<double>(neutralSampleCount - 1U);
            const auto value = std::exp2(
                -24.0 + (maximumInputEv + 24.0) * fraction);
            const auto sceneValue = static_cast<double>(
                static_cast<float>(value));
            const LinearRgb scene{sceneValue, sceneValue, sceneValue};
            const auto reference = MapAnalytic(
                algorithm, scene, peak, aces2);
            const auto sampled = SampleOperatorLut(lut, scene);
            statistics.Record(reference, sampled, sampleIndex);
        }
        statistics.Report(algorithm, peak, maximumBound);
    }

    void CheckNearBlack() const
    {
        double darkMaximumNits{};
        double darkMaximumPqCodes{};
        double darkTotalPqCodes{};
        const auto darkCorpus = test::BuildNearBlackOperatorCorpus();
        for (const auto scene : darkCorpus)
        {
            const auto reference = MapAnalytic(algorithm, scene, peak, aces2);
            const auto sampled = SampleOperatorLut(lut, scene);
            const std::array expected{reference.red, reference.green, reference.blue};
            const std::array actual{sampled.red, sampled.green, sampled.blue};
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                FO4HDR_CHECK(std::isfinite(actual[channel]) &&
                    actual[channel] >= 0.0 && actual[channel] <= peak);
                darkMaximumNits = std::max(darkMaximumNits,
                    std::abs(actual[channel] - expected[channel]));
                const auto codes = 1023.0 * std::abs(EncodeSt2084Pq(actual[channel]) -
                    EncodeSt2084Pq(expected[channel]));
                darkMaximumPqCodes = std::max(darkMaximumPqCodes, codes);
                darkTotalPqCodes += codes;
            }
        }
        std::fprintf(stdout, "near-black LUT CPU algorithm=%u peak=%.0f samples=%zu "
            "max_nits=%.9f max_pq_codes=%.9f mean_pq_codes=%.9f\n",
            static_cast<unsigned>(algorithm), peak, darkCorpus.size(), darkMaximumNits,
            darkMaximumPqCodes, darkTotalPqCodes / (darkCorpus.size() * 3.0));
        FO4HDR_CHECK(darkMaximumPqCodes <=
            (algorithm == ProductionToneMapperAlgorithm::aces2Lmt ? 3.0 : 32.0));
    }

    void CheckRepeatedBuild() const
    {
        const auto duplicate = BuildOperatorLut(algorithm, peak);
        FO4HDR_CHECK(duplicate.IsValid());
        FO4HDR_CHECK(duplicate.rgba16 == lut.rgba16);

        // Exercise sanitization independently from the finite corpus.
        // ACES2LMT maps non-finite input to zero; AgX/GT7 retain their
        // positive-infinity-to-finite-FP16-maximum input policy.
        const auto high = algorithm ==
            ProductionToneMapperAlgorithm::aces2Lmt ? 0.0 : 65504.0;
        CheckRgb(SampleOperatorLut(lut, {-1.0, 0.18, 1.0}),
            SampleOperatorLut(lut, {0.0, 0.18, 1.0}), 0.0);
        CheckRgb(SampleOperatorLut(lut, {nan, -infinity, infinity}),
            SampleOperatorLut(lut, {0.0, 0.0, high}), 0.0);

        // Reuse the coefficient data while checking that invalid metadata
        // is rejected.
        const auto checkInvalidField = [this](auto& field, auto bad) {
            const auto original = field;
            field = bad;
            FO4HDR_CHECK(!lut.IsValid());
            field = original;
        };
        checkInvalidField(lut.edge, 0U);
        checkInvalidField(lut.textureEdge, 0U);
        checkInvalidField(lut.inputOffset, 0.0);
        checkInvalidField(lut.inputShaperMinimum, 0.0);
        checkInvalidField(lut.inputShaperMaximum, 0.0);
        checkInvalidField(lut.displayPeakNits, nan);
        checkInvalidField(lut.algorithm, invalidAlgorithm);
        const auto alpha = lut.rgba16.back();
        lut.rgba16.pop_back();
        FO4HDR_CHECK(!lut.IsValid());
        lut.rgba16.push_back(alpha);
        FO4HDR_CHECK(lut.IsValid());
    }
};

void CheckOperatorLutCase(ProductionToneMapperAlgorithm algorithm, double peak)
{
    CheckShaperContract(algorithm, peak);
    auto lut = BuildOperatorLut(algorithm, peak);
    FO4HDR_CHECK(lut.IsValid());
    if (!lut.IsValid()) return;
    const auto aces2 = algorithm == ProductionToneMapperAlgorithm::aces2Lmt ?
        CreateAces2Transform(peak) : Aces2TransformHandle{};
    FO4HDR_CHECK(algorithm != ProductionToneMapperAlgorithm::aces2Lmt || aces2 != nullptr);
    if (algorithm == ProductionToneMapperAlgorithm::aces2Lmt && !aces2) return;
    const LutTestCase checks{algorithm, peak, lut, aces2.get()};
    checks.CheckMetadata();
    checks.CheckTexels();
    checks.CheckCorpus();
    checks.CheckNeutral();
    checks.CheckNearBlack();
    if (peak == 1000.0) checks.CheckRepeatedBuild();
}

void TestOperatorLutContractAndCorpus()
{
    TestInterpolationFixture();
    TestInvalidOperatorLuts();
    for (const auto algorithm : algorithms)
        for (const auto peak : {400.0, 430.0, 1000.0, 2000.0})
        {
            CheckOperatorLutCase(algorithm, peak);
        }
}

void TestOperatorLutCancellation()
{
    std::atomic<std::uint64_t> generation{7};
    FO4HDR_CHECK(!OperatorLutCancellation{}.IsCancelled());
    const OperatorLutCancellation cancellation{&generation, 7};
    FO4HDR_CHECK(!cancellation.IsCancelled());
    generation.store(8, std::memory_order_release);
    FO4HDR_CHECK(cancellation.IsCancelled());
    const auto discarded = BuildOperatorLut(
        ProductionToneMapperAlgorithm::agX, 1000.0, {}, cancellation);
    FO4HDR_CHECK(!discarded.IsValid());
    FO4HDR_CHECK(discarded.rgba16.empty());
}
}  // namespace

void RunOperatorLutTests()
{
    TestOperatorLutContractAndCorpus();
    TestOperatorLutCancellation();
}
}  // namespace fo4hdr::tests
