// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"

#include "Fallout4HDR/tonemap/Agx.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include "TestHarness.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

// Compile the exact, immutable Polyphony source only into this independent
// oracle. Its sample entry point is renamed; no upstream code enters the DLL.
#define main Fo4HdrGt7OfficialSampleMain
#include "../../../third_party/gt7/upstream/gt7_tone_mapping.cpp"
#undef main

namespace fo4hdr::tests {
namespace {

constexpr std::array kOfficialParityPeaks{
    250.0, 400.0, 430.0, 1000.0, 2000.0, 4000.0, 10000.0};

[[nodiscard]] std::uint32_t FloatDistance(float left, float right) noexcept
{
    const auto a = std::bit_cast<std::uint32_t>(left);
    const auto b = std::bit_cast<std::uint32_t>(right);
    return a > b ? a - b : b - a;
}

void CheckOfficialParity(
    LinearRgb input,
    double displayPeakNits)
{
    GT7ToneMapping oracle;
    oracle.initializeAsHDR(static_cast<float>(displayPeakNits));
    const std::array<float, 3> source{
        static_cast<float>(input.red),
        static_cast<float>(input.green),
        static_cast<float>(input.blue)
    };
    std::array<float, 3> expected{};
    oracle.applyToneMapping(source.data(), expected.data());

    const auto actual = MapGt7ReferenceRec2020(input, displayPeakNits);
    const std::array actualChannels{
        static_cast<float>(actual.red),
        static_cast<float>(actual.green),
        static_cast<float>(actual.blue)
    };
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        const auto distance = FloatDistance(
            actualChannels[channel], expected[channel]);
        if (distance > 2U)
        {
            std::cerr << "GT7 official CPU parity: peak=" << displayPeakNits
                      << " input={" << source[0] << ',' << source[1]
                      << ',' << source[2] << '}'
                      << " channel=" << channel
                      << " actual=" << actualChannels[channel]
                      << " expected=" << expected[channel]
                      << " float_ulp=" << distance << '\n';
        }
        FO4HDR_CHECK(std::isfinite(expected[channel]));
        FO4HDR_CHECK(std::isfinite(actualChannels[channel]));
        FO4HDR_CHECK(actualChannels[channel] >= 0.0F);
        FO4HDR_CHECK(actualChannels[channel] <=
            static_cast<float>(displayPeakNits) / 100.0F);
        FO4HDR_CHECK(distance <= 2U);
    }
}

void CheckOfficialAdapterParity(
    LinearRgb sceneLinear,
    double displayPeakNits,
    bool& scaledAboveNativeFp16)
{
    const auto sceneRec2020 = ConvertBt709ToBt2020(sceneLinear);
    constexpr float sceneScale =
        static_cast<float>(kSceneReferenceWhiteNits) / 100.0F;
    const std::array<float, 3> officialInput{
        static_cast<float>(sceneRec2020.red) * sceneScale,
        static_cast<float>(sceneRec2020.green) * sceneScale,
        static_cast<float>(sceneRec2020.blue) * sceneScale
    };
    scaledAboveNativeFp16 = scaledAboveNativeFp16 ||
        officialInput[0] > 65504.0F || officialInput[1] > 65504.0F ||
        officialInput[2] > 65504.0F;

    GT7ToneMapping oracle;
    oracle.initializeAsHDR(static_cast<float>(displayPeakNits));
    std::array<float, 3> expected{};
    oracle.applyToneMapping(officialInput.data(), expected.data());

    const auto mapped = MapGt7ToBt2020Nits(sceneLinear, displayPeakNits);
    const std::array<float, 3> actual{
        static_cast<float>(mapped.red / 100.0),
        static_cast<float>(mapped.green / 100.0),
        static_cast<float>(mapped.blue / 100.0)
    };
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        const auto distance = FloatDistance(actual[channel], expected[channel]);
        if (distance > 2U)
        {
            std::cerr << "GT7 official adapter parity: peak="
                      << displayPeakNits << " scene={" << sceneLinear.red
                      << ',' << sceneLinear.green << ',' << sceneLinear.blue
                      << "} official_input={" << officialInput[0] << ','
                      << officialInput[1] << ',' << officialInput[2] << '}'
                      << " channel=" << channel
                      << " actual=" << actual[channel]
                      << " expected=" << expected[channel]
                      << " float_ulp=" << distance << '\n';
        }
        FO4HDR_CHECK(std::isfinite(expected[channel]));
        FO4HDR_CHECK(std::isfinite(actual[channel]));
        FO4HDR_CHECK(actual[channel] >= 0.0F);
        FO4HDR_CHECK(actual[channel] <=
            static_cast<float>(displayPeakNits) / 100.0F);
        FO4HDR_CHECK(distance <= 2U);
    }
}

void TestOfficialReferenceVectorsAndPeaks()
{
    constexpr std::array<LinearRgb, 8> vectors{{
        {0.5, 1.23, 0.75},
        {12.3, 34.3, 56.9},
        {1504.7, 64.51, 0.5},
        {0.0, 0.0, 0.0},
        {0.18, 0.18, 0.18},
        {1.0, 1.0, 1.0},
        {8.0, 0.0, 0.0},
        {0.0, 8.0, 16.0}
    }};
    bool scaledAboveNativeFp16{};
    for (const auto peak : kOfficialParityPeaks)
    {
        for (const auto& input : vectors)
        {
            CheckOfficialParity(input, peak);
            CheckOfficialAdapterParity(
                input, peak, scaledAboveNativeFp16);
        }
    }

    const auto clipped = MapGt7ReferenceRec2020(
        {12.3, 34.3, 56.9}, 1000.0);
    FO4HDR_CHECK(clipped.red == 10.0);
    FO4HDR_CHECK(clipped.green == 10.0);
    FO4HDR_CHECK(clipped.blue == 10.0);
    const auto extreme = MapGt7ReferenceRec2020(
        {1504.7, 64.51, 0.5}, 1000.0);
    FO4HDR_CHECK(extreme.red == 10.0);
    FO4HDR_CHECK(extreme.green == 10.0);
    FO4HDR_CHECK(extreme.blue > 6.6 && extreme.blue < 6.8);

    // A zero source primary is not permission to erase genuine color-volume
    // cross-talk: both of these official HDR cases retain a positive red.
    for (const auto input : {
             LinearRgb{0.0, 0.60791015625, 54.21875},
             LinearRgb{0.0, 2.6875, 28.734375}})
    {
        const auto mapped = MapGt7ReferenceRec2020(input, 400.0);
        FO4HDR_CHECK(mapped.red > 1.0e-5);
        CheckOfficialParity(input, 400.0);
        CheckOfficialAdapterParity(input, 400.0, scaledAboveNativeFp16);
    }
}

// Check the exact upstream oracle both in native Rec.2020 and through the
// BT.709/203-nit adapter. LUT interpolation has a separate error budget.
struct OfficialParityCorpus
{
    double peak;
    bool& scaledAboveNativeFp16;

    void check(LinearRgb input) const
    {
        CheckOfficialParity(input, peak);
        CheckOfficialAdapterParity(input, peak, scaledAboveNativeFp16);
    }

    static constexpr std::array levels{
        0.0, 0x1p-24, 0x1p-12, 0.001, 0.01, 0.18, 0.538,
        1.0, 2.0, 4.0, 16.0, 64.0, 256.0, 4096.0, 65504.0};
    static constexpr std::array hueLevels{
        0x1p-12, 0.01, 0.18, 1.0, 4.0, 16.0, 256.0, 65504.0};

    void CheckGrid() const
    {
        // Full Cartesian combinations include asymmetric emissives, exact
        // zero-primary boundaries, and values up to the finite-input ceiling.
        for (const auto red : levels)
            for (const auto green : levels)
                for (const auto blue : levels)
                    check({red, green, blue});
    }

    void CheckNeutralRamp() const
    {
        // Dense near-black through extreme-highlight neutral ramp in
        // quarter-stop increments; no monotonicity assumption is imposed.
        for (int quarterStop = -96; quarterStop <= 63; ++quarterStop)
        {
            const auto value = std::exp2(quarterStop / 4.0);
            check({value, value, value});
        }
    }

    void CheckHues() const
    {
        // Six-sector hue sweeps at three saturations and eight intensities
        // exercise chroma directions independently of the Cartesian grid.
        for (int step = 0; step < 16; ++step)
        {
            const auto fraction = step / 16.0;
            const std::array hues{
                LinearRgb{1.0, fraction, 0.0},
                LinearRgb{1.0 - fraction, 1.0, 0.0},
                LinearRgb{0.0, 1.0, fraction},
                LinearRgb{0.0, 1.0 - fraction, 1.0},
                LinearRgb{fraction, 0.0, 1.0},
                LinearRgb{1.0, 0.0, 1.0 - fraction}};
            for (const auto hue : hues) CheckHue(hue);
        }
    }

    void CheckHue(LinearRgb hue) const
    {
        for (const auto saturation : {0.25, 0.75, 1.0})
            for (const auto intensity : hueLevels)
            {
                const auto channel = [=](double component) {
                    return intensity *
                        (1.0 - saturation + saturation * component);
                };
                check({channel(hue.red),
                    channel(hue.green), channel(hue.blue)});
            }
    }

    void CheckBoundary(float boundary) const
    {
        const std::array neighbors{
            std::nextafter(boundary, 0.0F), boundary,
            std::nextafter(boundary,
                std::numeric_limits<float>::infinity()),
            boundary * 0.9999F, boundary * 1.0001F};
        for (const auto value : neighbors)
        {
            if (value < 0.0F || value > 65504.0F) continue;
            check({value, value, value});
            check({value, 0.0, 0.0});
            check({0.0, value, 0.0});
            check({0.0, 0.0, value});
            check({value, value * 0.25, 0.0});
            check({0.0, value, value * 0.25});
            check({value * 0.25, 0.0, value});
        }
    }

    void CheckBoundaries() const
    {
        const auto framebufferPeak = static_cast<float>(peak) / 100.0F;
        for (const auto boundary : {
                 0.0F, 0.538F, 0.444F * framebufferPeak,
                 framebufferPeak, 65504.0F})
            CheckBoundary(boundary);

        // Locate the neutral chroma-fade thresholds using the untouched
        // sample's PQ helpers. Its inverse is clamped to PQ <= 1, so only
        // thresholds inside that invertible range are constructed this way.
        const auto peakIntensity = inverseEotfSt2084(framebufferPeak);
        for (const auto fade : {0.98F, 1.16F})
        {
            const auto encoded = fade * peakIntensity;
            if (encoded < 1.0F)
                CheckBoundary(eotfSt2084(encoded));
        }
    }
};

void TestBroadOfficialSourceParity()
{
    // Every sample exercises both the unchanged oracle's native Rec.2020
    // input and the scene-linear BT.709/203-nit adapter.
    bool scaledAboveNativeFp16{};
    for (const auto peak : kOfficialParityPeaks)
    {
        const OfficialParityCorpus corpus{peak, scaledAboveNativeFp16};
        corpus.CheckGrid();
        corpus.CheckNeutralRamp();
        corpus.CheckHues();
        corpus.CheckBoundaries();
    }
    FO4HDR_CHECK(scaledAboveNativeFp16);
}

void TestDisplayAdapterAndGamutBoundaries()
{
    constexpr std::array<LinearRgb, 11> corpus{{
        {0.0, 0.0, 0.0},
        {0.001, 0.001, 0.001},
        {0.01, 0.01, 0.01},
        {0.18, 0.18, 0.18},
        {1.0, 1.0, 1.0},
        {4.0, 4.0, 4.0},
        {8.0, 0.0, 0.0},
        {0.0, 8.0, 0.0},
        {0.0, 0.0, 8.0},
        {8.0, 8.0, 0.0},
        {2.03125, 0.6875, 0.53125}
    }};
    std::array<double, 3> highlights{};
    std::size_t peakIndex{};
    for (const auto peak : {400.0, 1000.0, 2000.0})
    {
        for (const auto& input : corpus)
        {
            const auto mapped = MapGt7ToBt709NitsM12(input, peak);
            for (const auto channel :
                 {mapped.red, mapped.green, mapped.blue})
            {
                FO4HDR_CHECK(std::isfinite(channel));
                FO4HDR_CHECK(channel >= 0.0);
                FO4HDR_CHECK(channel <= peak);
            }
        }

        const auto gray = MapGt7ToBt709NitsM12(
            {0.18, 0.18, 0.18}, peak);
        FO4HDR_CHECK(gray.red > 35.0 && gray.red < 37.0);
        FO4HDR_CHECK(std::abs(gray.red - gray.green) < 0.1);
        FO4HDR_CHECK(std::abs(gray.green - gray.blue) < 0.1);
        const auto white = MapGt7ToBt709NitsM12({1.0, 1.0, 1.0}, peak);
        FO4HDR_CHECK(white.red > 190.0 && white.red <= 205.0);

        const auto highlight = MapGt7ToBt709NitsM12(
            {4.0, 4.0, 4.0}, peak);
        highlights[peakIndex++] = highlight.red;
        FO4HDR_CHECK(highlight.red > white.red);
        const auto relative = highlight.red / kSceneReferenceWhiteNits;
        FO4HDR_CHECK(test::Approx(
            DecodeTemporalIntermediate(EncodeTemporalIntermediate(relative)),
            relative,
            1.0e-10));
    }
    FO4HDR_CHECK(highlights[0] < highlights[1]);
    FO4HDR_CHECK(highlights[1] < highlights[2]);
}

void TestComparisonGamutEmissives()
{
    bool outsideBt709Observed{};
    for (const auto& emissive : {
             LinearRgb{8.0, 0.0, 0.0},
             LinearRgb{0.0, 8.0, 0.0},
             LinearRgb{0.0, 0.0, 8.0}})
    {
        const auto mapped = MapGt7ToBt709NitsM12(emissive, 1000.0);
        FO4HDR_CHECK(mapped.red <= 1000.0);
        FO4HDR_CHECK(mapped.green <= 1000.0);
        FO4HDR_CHECK(mapped.blue <= 1000.0);
        const auto native = ConvertBt2020ToBt709ForComparison(
            MapGt7ToBt2020Nits(emissive, 1000.0));
        outsideBt709Observed = outsideBt709Observed ||
            native.red < 0.0 || native.green < 0.0 || native.blue < 0.0 ||
            native.red > 1000.0 || native.green > 1000.0 ||
            native.blue > 1000.0;
    }
    FO4HDR_CHECK(outsideBt709Observed);
}

void CheckBlack(LinearRgb value)
{
    FO4HDR_CHECK(value.red == 0.0);
    FO4HDR_CHECK(value.green == 0.0);
    FO4HDR_CHECK(value.blue == 0.0);
}

void TestInvalidInputsAndIndependentMode()
{
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    for (const auto invalid : {
             nan, infinity, -infinity, -1.0, 0.0, 249.0, 10001.0})
    {
        const auto raw = MapGt7ReferenceRec2020(
            {1.0, 1.0, 1.0}, invalid);
        const auto mapped = MapGt7ToBt709NitsM12(
            {1.0, 1.0, 1.0}, invalid);
        CheckBlack(raw);
        CheckBlack(mapped);
    }

    const auto black = MapGt7ToBt709NitsM12(
        {nan, -infinity, -1.0}, 1000.0);
    CheckBlack(black);
    const auto clipped = MapGt7ToBt709NitsM12(
        {infinity, infinity, infinity}, 1000.0);
    // The official ICtCp inverse uses rounded coefficients, so even a neutral
    // saturated source need not land on three bit-identical peak boundaries.
    for (const auto channel :
         {clipped.red, clipped.green, clipped.blue})
    {
        FO4HDR_CHECK(std::isfinite(channel));
        FO4HDR_CHECK(channel > 990.0);
        FO4HDR_CHECK(channel <= 1000.0);
    }
}

void TestIndependentMode()
{
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    constexpr LinearRgb emissive{2.03125, 0.6875, 0.53125};
    const auto gt7 = MapGt7ToBt709NitsM12(emissive, 1000.0);
    const auto agx = ConvertBt2020ToBt709ForComparison(
        MapAgxToBt2020Nits(emissive, 1000.0));
    FO4HDR_CHECK(std::abs(gt7.red - agx.red) > 1.0 ||
        std::abs(gt7.green - agx.green) > 1.0 ||
        std::abs(gt7.blue - agx.blue) > 1.0);

    const auto contained = ContainBt2020Nits(
        {nan, infinity, -1.0}, 1000.0);
    FO4HDR_CHECK(contained.red == 0.0);
    FO4HDR_CHECK(contained.green == 1000.0);
    FO4HDR_CHECK(contained.blue == 0.0);
}

void CheckParameterCorner(double peak, const ToneMapperParameters& parameters)
{
    constexpr std::array neutralRamp{
        0.0, 0.001, 0.01, 0.18, 1.0, 4.0, 16.0, 65504.0};
    constexpr std::array saturated{
        LinearRgb{65504.0, 0.0, 0.0},
        LinearRgb{0.0, 65504.0, 0.0},
        LinearRgb{0.0, 0.0, 65504.0},
        LinearRgb{65504.0, 1.0, 0.001}};

    const auto gray = MapGt7ToBt2020Nits(
        {0.18, 0.18, 0.18}, peak, parameters);
    FO4HDR_CHECK(gray.red > 0.0 && gray.red < peak);
    FO4HDR_CHECK(std::abs(gray.red - gray.green) < 0.1);
    FO4HDR_CHECK(std::abs(gray.green - gray.blue) < 0.1);

    double previous{-1.0};
    for (const auto level : neutralRamp)
    {
        const auto mapped = MapGt7ToBt2020Nits(
            {level, level, level}, peak, parameters);
        FO4HDR_CHECK(mapped.red + 1.0e-4 >= previous);
        FO4HDR_CHECK(std::isfinite(mapped.red));
        previous = mapped.red;
    }
    for (const auto input : saturated)
    {
        const auto mapped = MapGt7ToBt2020Nits(
            input, peak, parameters);
        for (const auto channel :
             {mapped.red, mapped.green, mapped.blue})
        {
            FO4HDR_CHECK(std::isfinite(channel));
            FO4HDR_CHECK(channel >= 0.0 && channel <= peak);
        }
    }
}

void TestParameterCorners()
{
    for (const auto peak : {400.0, 1000.0, 2000.0})
        for (const auto toe :
             {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength})
            for (const auto shoulder :
                 {kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart})
                for (const auto blend :
                     {kMinimumGt7ColorBlend, kMaximumGt7ColorBlend})
                {
                    ToneMapperParameters parameters{};
                    parameters.gt7ToeStrength = toe;
                    parameters.gt7ShoulderStart = shoulder;
                    parameters.gt7ColorBlend = blend;

                    CheckParameterCorner(peak, parameters);
                }
}

void TestInvalidParametersFailBlack()
{
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    const auto check = [](const ToneMapperParameters& parameters) {
        FO4HDR_CHECK(!CreateGt7Transform(1000.0, parameters));
        const auto mapped = MapGt7ToBt2020Nits(
            {1.0, 1.0, 1.0}, 1000.0, parameters);
        CheckBlack(mapped);
    };

    for (const auto invalid : {nan, infinity,
             kMinimumGt7ToeStrength - 0.01,
             kMaximumGt7ToeStrength + 0.01})
    {
        ToneMapperParameters parameters{};
        parameters.gt7ToeStrength = invalid;
        check(parameters);
    }
    for (const auto invalid : {nan, infinity,
             kMinimumGt7ShoulderStart - 0.01,
             kMaximumGt7ShoulderStart + 0.01})
    {
        ToneMapperParameters parameters{};
        parameters.gt7ShoulderStart = invalid;
        check(parameters);
    }
    for (const auto invalid : {nan, infinity,
             kMinimumGt7ColorBlend - 0.01,
             kMaximumGt7ColorBlend + 0.01})
    {
        ToneMapperParameters parameters{};
        parameters.gt7ColorBlend = invalid;
        check(parameters);
    }
}

struct ConfiguredParitySample
{
    double peak;
    const ToneMapperParameters& parameters;
    const Gt7Transform& prepared;
    GT7ToneMapping& oracle;

    void Check(LinearRgb input) const
    {
        const auto actual = EvaluateGt7LutNits(prepared, input);
        const auto direct = EvaluateGt7LutNits(input, peak, parameters);
        FO4HDR_CHECK(actual.red == direct.red &&
            actual.green == direct.green && actual.blue == direct.blue);

        const auto rec2020 = ConvertBt709ToBt2020(input);
        constexpr float scale = 203.0F / 100.0F;
        const std::array source{
            static_cast<float>(rec2020.red) * scale,
            static_cast<float>(rec2020.green) * scale,
            static_cast<float>(rec2020.blue) * scale};
        std::array<float, 3> expected{};
        oracle.applyToneMapping(source.data(), expected.data());
        const auto contained = ContainBt2020Nits(actual, peak);
        const std::array channels{contained.red, contained.green,
            contained.blue};
        for (std::size_t channel = 0; channel < 3; ++channel)
            FO4HDR_CHECK(FloatDistance(
                static_cast<float>(channels[channel] / 100.0),
                expected[channel]) <= 2U);
    }
};

void CheckPreparedConfiguredSource(double peak, const ToneMapperParameters& parameters)
{
    constexpr std::array levels{
        0.0, 0x1p-24, 0.001, 0.01, 0.18, 0.538, 1.0, 4.0, 64.0, 65504.0};
    const auto prepared = CreateGt7Transform(peak, parameters);
    FO4HDR_CHECK(prepared != nullptr);
    if (!prepared) return;

    GT7ToneMapping oracle;
    oracle.initializeAsHDR(static_cast<float>(peak));
    oracle.curve_.initializeCurve(static_cast<float>(peak) / 100.0F,
        0.25F, 0.538F, static_cast<float>(parameters.gt7ShoulderStart),
        static_cast<float>(parameters.gt7ToeStrength));
    oracle.blendRatio_ = static_cast<float>(parameters.gt7ColorBlend);
    const ConfiguredParitySample parity{peak, parameters, *prepared, oracle};
    for (const auto red : levels)
        for (const auto green : levels)
            for (const auto blue : levels)
            {
                const LinearRgb input{red, green, blue};
                parity.Check(input);
            }
}

void TestPreparedConfiguredSourceParity()
{
    for (const auto peak : {400.0, 430.0, 1000.0, 2000.0})
        for (const auto toe : {kMinimumGt7ToeStrength, kMaximumGt7ToeStrength})
            for (const auto shoulder :
                 {kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart})
                for (const auto blend :
                     {kMinimumGt7ColorBlend, 0.6, kMaximumGt7ColorBlend})
                {
                    ToneMapperParameters parameters{};
                    parameters.gt7ToeStrength = toe;
                    parameters.gt7ShoulderStart = shoulder;
                    parameters.gt7ColorBlend = blend;
                    CheckPreparedConfiguredSource(peak, parameters);
                }
    for (const auto peak : {0.0, 249.0, 10001.0,
             std::numeric_limits<double>::quiet_NaN()})
        FO4HDR_CHECK(!CreateGt7Transform(peak));
}
}  // namespace

void RunGt7Tests()
{
    TestOfficialReferenceVectorsAndPeaks();
    TestBroadOfficialSourceParity();
    TestDisplayAdapterAndGamutBoundaries();
    TestComparisonGamutEmissives();
    TestInvalidInputsAndIndependentMode();
    TestIndependentMode();
    TestParameterCorners();
    TestInvalidParametersFailBlack();
    TestPreparedConfiguredSourceParity();
}
}  // namespace fo4hdr::tests
