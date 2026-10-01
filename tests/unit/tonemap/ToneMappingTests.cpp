// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "reference/Aces2LmtSourceOracle.hpp"

#include "FO4Isotope/tonemap/Aces2.hpp"
#include "FO4Isotope/tonemap/Aces2Lmt.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace fo4hdr::tests {
namespace {

void CheckRgb(LinearRgb actual, LinearRgb expected, double tolerance)
{
    FO4HDR_CHECK(test::Approx(actual.red, expected.red, tolerance));
    FO4HDR_CHECK(test::Approx(actual.green, expected.green, tolerance));
    FO4HDR_CHECK(test::Approx(actual.blue, expected.blue, tolerance));
}

void CheckContainedRgb(LinearRgb value, double peak)
{
    for (const auto channel : {value.red, value.green, value.blue})
    {
        FO4HDR_CHECK(std::isfinite(channel));
        FO4HDR_CHECK(channel >= 0.0 && channel <= peak);
    }
}

void TestIndependentAces2LmtSourcePrecision()
{
    // The test oracle retains the pinned DCTL's source-float evaluation while
    // production deliberately uses double precision. This isolates the LMT
    // stage from the already independently anchored Academy Output Transform.
    constexpr std::array inputs{
        LinearRgb{0.0, 0.0, 0.0},
        LinearRgb{0.18, 0.18, 0.18},
        LinearRgb{1.0, 1.0, 1.0},
        LinearRgb{8.0, 0.0, 0.0},
        LinearRgb{0.0, 8.0, 0.0},
        LinearRgb{0.0, 0.0, 8.0},
        LinearRgb{2.0, 0.7, 0.5},
        LinearRgb{-0.01, 0.18, 1.0},
        LinearRgb{64.0, 22.4, 0.0}};
    for (const auto input : inputs)
    {
        const auto source = ApplyAces2ContrastLmtSourceFloat(input);
        const auto production = ApplyAces2ContrastLmt(input);
        const auto tolerance = 2.0e-4 * std::max({1.0,
            std::abs(source.red), std::abs(source.green),
            std::abs(source.blue)});
        CheckRgb(production, source, tolerance);
    }

    const auto direct = CreateAces2Transform(1000.0);
    FO4HDR_CHECK(direct != nullptr);
    if (direct)
    {
        const auto scene = LinearRgb{2.0, 0.7, 0.5};
        const auto unchanged = MapAces2ToBt2020Nits(*direct, scene);
        const auto looked = MapAdaptedAces2ToBt2020Nits(*direct, scene);
        FO4HDR_CHECK(std::max({std::abs(unchanged.red - looked.red),
            std::abs(unchanged.green - looked.green),
            std::abs(unchanged.blue - looked.blue)}) > 0.01);
    }
}

void CheckSourcePrecision(LinearRgb actual, LinearRgb expected)
{
    // This is an absolute-nit oracle budget. test::Approx is relative and
    // would inadvertently allow two nits of error at a 1000-nit output.
    FO4HDR_CHECK(std::abs(actual.red - expected.red) <= 0.002);
    FO4HDR_CHECK(std::abs(actual.green - expected.green) <= 0.002);
    FO4HDR_CHECK(std::abs(actual.blue - expected.blue) <= 0.002);
}

void CheckAces2PeakReferences()
{
    // Independent OCIO 2.4.2 values cover custom/preset peaks, hue-interval
    // boundaries, and the full finite FP16 scene endpoint.
    struct PeakReference
    {
        double peak;
        LinearRgb input;
        LinearRgb output;
    };
    constexpr std::array peakReferences{
        PeakReference{400.0, {0.18, 0.18, 0.18},
            {12.757924199104, 12.757939100266, 12.757928669453}},
        PeakReference{400.0, {64.0, 22.4, 0.0},
            {399.656343460083, 361.354207992554, 290.797209739685}},
        PeakReference{400.0, {65504.0, 0.0, 0.0},
            {400.0, 400.0, 400.0}},
        PeakReference{430.0, {0.18, 0.18, 0.18},
            {12.899301946163, 12.899318337440, 12.899309396744}},
        PeakReference{430.0, {64.0, 22.4, 0.0},
            {429.135084152222, 386.098241806030, 307.210040092468}},
        PeakReference{430.0, {64.0, 0.0, 57.6},
            {422.561836242676, 333.740019798279, 428.874254226685}},
        PeakReference{430.0, {65504.0, 0.0, 0.0},
            {430.0, 430.0, 430.0}},
        PeakReference{500.0, {0.18, 0.18, 0.18},
            {13.192866742611, 13.192880153656, 13.192871212959}},
        PeakReference{500.0, {64.0, 22.4, 0.0},
            {497.404384613037, 442.806100845337, 343.788266181946}},
        PeakReference{500.0, {65504.0, 0.0, 0.0},
            {500.0, 500.0, 500.0}},
        PeakReference{1000.0, {64.0, 22.4, 0.0},
            {957.717037200928, 812.109851837158, 561.223602294922}},
        PeakReference{1000.0, {65504.0, 0.0, 0.0},
            {1000.0, 1000.0, 1000.0}},
        PeakReference{2000.0, {0.18, 0.18, 0.18},
            {15.746709704399, 15.746742486954, 15.746729075909}},
        PeakReference{2000.0, {64.0, 22.4, 0.0},
            {1768.410110473633, 1413.102912902832, 840.102100372314}},
        PeakReference{2000.0, {65504.0, 0.0, 0.0},
            {2000.0, 2000.0, 2000.0}},
        // This blue boundary is discontinuous in OCIO itself. Preserve both
        // sides as source behavior; LUT interpolation must not hide the jump.
        PeakReference{2000.0, {0.0, 0.0, 21.33},
            {88.756132125854, 0.0, 2000.0}},
        PeakReference{2000.0, {0.0, 0.0, 21.34},
            {106.985712051392, 30.772495269775, 2000.0}}
    };
    for (const auto& reference : peakReferences)
        CheckSourcePrecision(
            MapAces2ToBt2020Nits(reference.input, reference.peak), reference.output);
}

void TestIndependentAces2OcioReferenceVectors()
{
    struct Reference
    {
        LinearRgb input;
        LinearRgb output;
    };
    constexpr std::array references{
        Reference{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
        Reference{{0.18, 0.18, 0.18},
            {14.511531591415, 14.511562883854, 14.511547982693}},
        Reference{{0.97784, 0.97784, 0.97784},
            {104.156661033630, 104.156637191772, 104.156625270844}},
        Reference{{8.0, 0.0, 0.0},
            {468.879699707031, 84.470617771149, 30.910950899124}},
        Reference{{0.0, 8.0, 0.0},
            {292.978191375732, 543.024587631226, 158.105611801147}},
        Reference{{0.0, 0.0, 8.0},
            {30.586963891983, 0.572490692139, 756.818771362305}},
        Reference{{2.03125, 0.6875, 0.53125},
            {145.051610469818, 89.921796321869, 72.836506366730}}
    };
    const auto transform = CreateAces2Transform(1000.0);
    FO4HDR_CHECK(transform != nullptr);
    if (!transform) return;
    for (const auto& reference : references)
        CheckSourcePrecision(
            MapAces2ToBt2020Nits(*transform, reference.input), reference.output);

    CheckAces2PeakReferences();
}

struct Crossing
{
    double strength;
    double lower;
    double upper;
};

void CheckCompressionCrossing(const Aces2Transform& transform, Crossing crossing)
{
    double lower = crossing.lower;
    double upper = crossing.upper;
    auto lowerState = EvaluateAdaptedAces2CompressionBoundary(
        transform, {0, 0, lower}, crossing.strength);
    const auto upperState = EvaluateAdaptedAces2CompressionBoundary(
        transform, {0, 0, upper}, crossing.strength);
    FO4HDR_CHECK(lowerState.valid && upperState.valid);
    FO4HDR_CHECK(lowerState.chromaMinusGamut > 0 && upperState.chromaMinusGamut > 0);
    FO4HDR_CHECK((lowerState.gamutMinusReach >= 0) != (upperState.gamutMinusReach >= 0));
    for (int iteration = 0; iteration < 40; ++iteration) {
        const double middle = (lower + upper) * 0.5;
        const auto state = EvaluateAdaptedAces2CompressionBoundary(
            transform, {0, 0, middle}, crossing.strength);
        FO4HDR_CHECK(state.valid);
        if ((lowerState.gamutMinusReach >= 0) == (state.gamutMinusReach >= 0)) {
            lower = middle;
            lowerState = state;
        } else {
            upper = middle;
        }
    }
    const auto before = EvaluateAdaptedAces2LutNits(
        transform, {0, 0, lower}, crossing.strength);
    const auto after = EvaluateAdaptedAces2LutNits(
        transform, {0, 0, upper}, crossing.strength);
    // Check uncontained outputs: final channel clipping must not hide a jump.
    FO4HDR_CHECK(std::abs(before.red - after.red) < 1e-6);
    FO4HDR_CHECK(std::abs(before.green - after.green) < 1e-6);
    FO4HDR_CHECK(std::abs(before.blue - after.blue) < 1e-6);
}

void TestAces2ContinuousColor()
{
    // These positive AP0 inputs land exactly on the vertical opponent axis.
    // The pinned float oracle retains its original branch; this regression
    // checks the local correction against both neighboring hue directions.
    constexpr std::array axisInputs{
        LinearRgb{0.11031731507216315, 0.1, 0.016},
        LinearRgb{0.099842353783486118, 0.1, 0.113}};
    for (const auto input : axisInputs) {
        const auto center = ApplyAces2ContrastLmt(input);
        for (const double delta : {-1e-10, 1e-10}) {
            const auto neighbor = ApplyAces2ContrastLmt(
                {input.red + delta, input.green, input.blue});
            CheckRgb(center, neighbor, 1e-8);
        }
    }

    const auto transform = CreateAces2Transform(2000.0);
    FO4HDR_CHECK(transform != nullptr);
    if (!transform) return;
    constexpr std::array crossings{
        Crossing{0.0, 21.3395, 21.34},
        Crossing{0.5, 10.282, 10.2825},
        Crossing{1.0, 7.5915, 7.592}};
    for (const auto crossing : crossings) {
        CheckCompressionCrossing(*transform, crossing);
    }

    const LinearRgb axisScene{0.14804731352493253, 0.1, 0.004};
    for (const double strength : {0.0, 0.5, 1.0}) {
        const auto center = EvaluateAdaptedAces2LutNits(*transform, axisScene, strength);
        for (const double delta : {-1e-10, 1e-10})
            CheckRgb(center, EvaluateAdaptedAces2LutNits(*transform,
                {axisScene.red + delta, axisScene.green, axisScene.blue}, strength), 1e-8);
    }
}

void TestAces2TransformAndContainment()
{
    constexpr std::array corpus{
        LinearRgb{0.0, 0.0, 0.0}, LinearRgb{0.001, 0.001, 0.001},
        LinearRgb{0.01, 0.01, 0.01}, LinearRgb{0.18, 0.18, 0.18},
        LinearRgb{1.0, 1.0, 1.0}, LinearRgb{4.0, 4.0, 4.0},
        LinearRgb{8.0, 0.0, 0.0}, LinearRgb{0.0, 8.0, 0.0},
        LinearRgb{0.0, 0.0, 8.0}, LinearRgb{16.0, 2.0, 0.1}
    };

    for (const auto peak : {400.0, 430.0, 500.0, 1000.0, 2000.0})
    {
        const auto transform = CreateAces2Transform(peak);
        FO4HDR_CHECK(transform != nullptr);
        if (!transform) continue;

        double previous{-1.0};
        for (const auto sample :
             {0.0, 0.001, 0.01, 0.18, 1.0, 2.0, 4.0, 8.0, 16.0})
        {
            const LinearRgb input{sample, sample, sample};
            const auto retained = MapAces2ToBt2020Nits(*transform, input);
            const auto convenience = MapAces2ToBt2020Nits(input, peak);
            CheckRgb(retained, convenience, 1.0e-12);
            FO4HDR_CHECK(retained.red + 1.0e-8 >= previous);
            previous = retained.red;
        }

        for (const auto input : corpus)
        {
            const auto mapped = MapAces2ToBt2020Nits(*transform, input);
            CheckContainedRgb(mapped, peak);
        }
    }

    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    const auto sanitized = MapAces2ToBt2020Nits(
        {nan, -infinity, infinity}, 1000.0);
    CheckContainedRgb(sanitized, 1000.0);
    for (const auto invalid :
         {nan, infinity, -infinity, 0.0, 399.0, 2001.0})
    {
        FO4HDR_CHECK(CreateAces2Transform(invalid) == nullptr);
        CheckRgb(MapAces2ToBt2020Nits({1.0, 1.0, 1.0}, invalid), {}, 0.0);
    }
}

void CheckLookInputs(const Aces2Transform& transform, double peak)
{
    constexpr std::array inputs{
        LinearRgb{0, 0, 0}, LinearRgb{0.18, 0.18, 0.18},
        LinearRgb{0.0001, 0.00002, 0.0003}, LinearRgb{0.01, 0.03, 0.001},
        LinearRgb{2, 0.4, 0.05}, LinearRgb{0, 0.1, 8},
        LinearRgb{16, 4, 1}, LinearRgb{65504, 65504, 65504}};
    for (const auto input : inputs) {
        CheckRgb(MapAdaptedAces2ToBt2020Nits(transform, input, 1),
            MapAdaptedAces2ToBt2020Nits(transform, input), 0);
        for (const double strength : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            const auto mapped = MapAdaptedAces2ToBt2020Nits(transform, input, strength);
            CheckContainedRgb(mapped, peak);
        }
    }
}

void CheckLookRamps(const Aces2Transform& transform, double peak)
{
    for (const double strength : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        CheckRgb(MapAdaptedAces2ToBt2020Nits(transform, {}, strength), {}, 0);
        double previous{};
        for (int i = 0; i <= 4096; ++i) {
            const double y = std::exp2(-24.0 + i * 40.0 / 4096);
            const auto mapped = MapAdaptedAces2ToBt2020Nits(transform, {y, y, y}, strength);
            CheckContainedRgb(mapped, peak);
            FO4HDR_CHECK(mapped.red + 1e-7 >= previous);
            FO4HDR_CHECK(std::abs(mapped.red - mapped.green) <= peak * 1e-5);
            FO4HDR_CHECK(std::abs(mapped.red - mapped.blue) <= peak * 1e-5);
            previous = mapped.red;
        }
        // Each Look amount moves the internal transition relative to scene
        // gray. Dense scene samples cover both sides for every amount.
        previous = 0.0;
        for (int i = 0; i <= 2048; ++i) {
            const double y = 0.05 + i * 0.2 / 2048;
            const auto mapped = MapAdaptedAces2ToBt2020Nits(transform, {y, y, y}, strength);
            FO4HDR_CHECK(std::isfinite(mapped.red));
            FO4HDR_CHECK(mapped.red + 1e-7 >= previous);
            previous = mapped.red;
        }
    }
}

void CheckLookSemantics(const Aces2Transform& transform)
{
    for (const double y : {0.181, 0.5, 1.0, 8.0}) {
        const LinearRgb input{y, y, y};
        CheckRgb(EvaluateAdaptedAces2LutNits(transform, input, 0),
            EvaluateAces2LutNits(transform, input), 0);
    }
    const LinearRgb shadow{0.18 / 64, 0.18 / 64, 0.18 / 64};
    FO4HDR_CHECK(MapAdaptedAces2ToBt2020Nits(transform, shadow, 0).red >
        MapAces2ToBt2020Nits(transform, shadow).red * 10.0);
    const LinearRgb chromatic{2, 0.4, 0.05};
    const auto noLook = MapAdaptedAces2ToBt2020Nits(transform, chromatic, 0);
    const auto halfLook = MapAdaptedAces2ToBt2020Nits(transform, chromatic, 0.5);
    const auto fullLook = MapAdaptedAces2ToBt2020Nits(transform, chromatic, 1);
    FO4HDR_CHECK(std::abs(halfLook.red - noLook.red) > 1e-3);
    FO4HDR_CHECK(std::abs(halfLook.red - fullLook.red) > 1e-3);
    // A scene-linear Look blend must not become a blend of rendered outputs.
    FO4HDR_CHECK(std::abs(halfLook.red - (noLook.red + fullLook.red) * 0.5) > 1e-3);
    CheckRgb(MapAdaptedAces2ToBt2020Nits(transform,
            {-1, std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity()}, 0.5),
        MapAdaptedAces2ToBt2020Nits(transform, {}, 0.5), 0);
    for (const double invalid : {-0.01, 1.01,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity()}) {
        CheckRgb(MapAdaptedAces2ToBt2020Nits(transform, {1, 1, 1}, invalid), {}, 0);
        FO4HDR_CHECK(!EvaluateAdaptedAces2CompressionBoundary(
            transform, {0, 0, 8}, invalid).valid);
    }
}

void TestAces2LookStrength()
{
    for (const double peak : {400.0, 418.0, 430.0, 1000.0, 2000.0}) {
        const auto transform = CreateAces2Transform(peak);
        FO4HDR_CHECK(transform != nullptr);
        if (!transform) continue;
        CheckLookInputs(*transform, peak);
        CheckLookRamps(*transform, peak);
        CheckLookSemantics(*transform);
    }

    // Local scalar-adaptation regressions, separate from the unchanged OCIO
    // and source-float oracles above. Values are absolute display-linear nits.
    const auto transform = CreateAces2Transform(418.0);
    FO4HDR_CHECK(transform != nullptr);
    if (!transform) return;
    struct ShadowReference
    {
        int stops;
        std::array<double, 3> luminance;
    };
    constexpr std::array shadowReferences{
        ShadowReference{-8, {0.02317137675, 0.02280762831, 0.02244463193}},
        ShadowReference{-6, {0.1139578076, 0.1127673634, 0.1115785027}},
        ShadowReference{-4, {0.5549582792, 0.5548310748, 0.5547038731}},
        ShadowReference{-2, {2.549380815, 2.666880441, 2.784687172}},
        ShadowReference{0, {12.84402477, 16.32943947, 19.84860604}},
        ShadowReference{2, {61.97780801, 88.25947097, 111.3253758}}};
    for (const auto& reference : shadowReferences) {
        const double y = 0.18 * std::exp2(reference.stops);
        for (std::size_t index = 0; index < reference.luminance.size(); ++index) {
            const auto mapped = MapAdaptedAces2ToBt2020Nits(
                *transform, {y, y, y}, static_cast<double>(index) * 0.5);
            const double luminance = mapped.red * 0.2627 +
                mapped.green * 0.6780 + mapped.blue * 0.0593;
            FO4HDR_CHECK(std::abs(luminance - reference.luminance[index]) <= 1e-6);
        }
    }
}

[[nodiscard]] std::string ProductionConfiguration()
{
    return R"ini([General]
HdrEnabled=1
DeveloperMode=0
[Output]
DisplayPeakNits=1000
UiPaperWhiteNits=203
[ToneMapper]
Algorithm=ACES2
[Image]
ExposureEV=0
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
}

void Replace(std::string& text, std::string_view before,
    std::string_view after)
{
    const auto position = text.find(before);
    FO4HDR_CHECK(position != std::string::npos);
    if (position != std::string::npos)
        text.replace(position, before.size(), after);
}

void CheckUnsupportedAlgorithm(std::string_view algorithm)
{
    auto text = ProductionConfiguration();
    Replace(text, "Algorithm=ACES2", "Algorithm=" + std::string(algorithm));
    const auto parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::unsupportedAlgorithm);
}

void CheckConfigurationPeak()
{
    std::string text;
    PluginConfigurationResult parsed;
    for (const auto peak : {"400", "2000"})
    {
        text = ProductionConfiguration();
        Replace(text, "DisplayPeakNits=1000",
            "DisplayPeakNits=" + std::string(peak));
        FO4HDR_CHECK(ParsePluginConfiguration(text).valid);
    }
    for (const auto peak : {"399", "2001", "-1"})
    {
        text = ProductionConfiguration();
        Replace(text, "DisplayPeakNits=1000",
            "DisplayPeakNits=" + std::string(peak));
        parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(!parsed.valid);
        FO4HDR_CHECK(parsed.error ==
            PluginConfigurationError::valueOutOfRange);
    }

    for (const auto invalid : {"nan", "inf", "1000junk"})
    {
        text = ProductionConfiguration();
        Replace(text, "DisplayPeakNits=1000",
            "DisplayPeakNits=" + std::string(invalid));
        parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(!parsed.valid);
        FO4HDR_CHECK(parsed.error ==
            PluginConfigurationError::invalidNumber);
    }
    text = ProductionConfiguration();
    Replace(text, "UiPaperWhiteNits=203", "UiPaperWhiteNits=99");
    parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::valueOutOfRange);
}

void CheckConfigurationExposure()
{
    std::string text;
    PluginConfigurationResult parsed;
    for (const auto exposure : {
             std::pair{"-4", 0.0625}, std::pair{"0", 1.0},
             std::pair{"4", 16.0}})
    {
        text = ProductionConfiguration();
        Replace(text, "ExposureEV=0",
            "ExposureEV=" + std::string(exposure.first));
        parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(parsed.valid);
        FO4HDR_CHECK(test::Approx(parsed.value.sceneExposureMultiplier,
            exposure.second, 1.0e-15));
    }
    for (const auto invalid : {"-4.0001", "4.0001"})
    {
        text = ProductionConfiguration();
        Replace(text, "ExposureEV=0",
            "ExposureEV=" + std::string(invalid));
        parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(!parsed.valid);
        FO4HDR_CHECK(parsed.error ==
            PluginConfigurationError::valueOutOfRange);
    }

    text = ProductionConfiguration();
    Replace(text, "ExposureEV=0", "ExposureEV=0\nExposureEV=1");
    parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::duplicateKey);
    text = ProductionConfiguration();
    Replace(text, "ExposureEV=0\n", "");
    parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(parsed.error ==
        PluginConfigurationError::missingKey);
}

void CheckConfigurationModes()
{
    std::string text;
    PluginConfigurationResult parsed;
    constexpr std::string_view diagnosticOnly = R"ini(
[General]
DeveloperMode=1
[Debug]
TraceRenderPipeline=1
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(diagnosticOnly);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.productionRequested);
    FO4HDR_CHECK(parsed.value.sceneExposureMultiplier == 1.0);

    text = ProductionConfiguration();
    Replace(text, "HdrEnabled=1", "HdrEnabled=0");
    parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.productionRequested);
    FO4HDR_CHECK(ToString(static_cast<ProductionToneMapperAlgorithm>(0xFF)) ==
        "unknown");
}

void TestProductionConfiguration()
{
    auto text = ProductionConfiguration();
    auto parsed = ParsePluginConfiguration(text);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.enabled);
    FO4HDR_CHECK(parsed.value.productionRequested);
    FO4HDR_CHECK(parsed.value.productionAlgorithm ==
        ProductionToneMapperAlgorithm::aces2);
    FO4HDR_CHECK(ToString(parsed.value.productionAlgorithm) == "ACES2");
    FO4HDR_CHECK(parsed.value.displayPeakNits == 1000.0);
    FO4HDR_CHECK(parsed.value.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(parsed.value.exposureEv == 0.0);
    FO4HDR_CHECK(parsed.value.sceneExposureMultiplier == 1.0);

    for (const auto algorithm : {
             std::pair{"AgX", ProductionToneMapperAlgorithm::agX},
             std::pair{"GT7", ProductionToneMapperAlgorithm::gt7}})
    {
        text = ProductionConfiguration();
        Replace(text, "Algorithm=ACES2",
            "Algorithm=" + std::string(algorithm.first));
        parsed = ParsePluginConfiguration(text);
        FO4HDR_CHECK(parsed.valid);
        FO4HDR_CHECK(parsed.value.productionAlgorithm == algorithm.second);
        FO4HDR_CHECK(ToString(parsed.value.productionAlgorithm) ==
            algorithm.first);
    }

    for (const auto retiredOrUnknown : {
             "ACES2LMT", "ACES1", "ACESFitted", "AgXPunchy", "ACES",
             "ACES 2", "ACES2-LMT", "ACESLMT", "aces2",
             "aces2lmt", "agx", "gt7"})
        CheckUnsupportedAlgorithm(retiredOrUnknown);

    CheckConfigurationPeak();
    CheckConfigurationExposure();
    CheckConfigurationModes();
}
}  // namespace

void RunToneMappingTests()
{
    TestIndependentAces2OcioReferenceVectors();
    TestIndependentAces2LmtSourcePrecision();
    TestAces2ContinuousColor();
    TestAces2TransformAndContainment();
    TestAces2LookStrength();
    RunOperatorLutTests();
    TestProductionConfiguration();
}
}  // namespace fo4hdr::tests
