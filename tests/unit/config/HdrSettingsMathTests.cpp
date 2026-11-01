// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "TestHarness.hpp"
#include <Windows.h>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace fo4hdr::tests {

void TestLiveSettingsThreads()
{
    // Startup/load Presents may migrate before a production draw exists.
    FO4HDR_CHECK(LiveSettingsFrameThreadsCompatible(0, false, 101));
    FO4HDR_CHECK(LiveSettingsFrameThreadsCompatible(0, false, 202));
    FO4HDR_CHECK(LiveSettingsFrameThreadsCompatible(202, false, 202));
    FO4HDR_CHECK(!LiveSettingsFrameThreadsCompatible(101, false, 202));
    FO4HDR_CHECK(!LiveSettingsFrameThreadsCompatible(202, true, 202));
    // A later compatible frame recovers after a prior mismatch.
    FO4HDR_CHECK(LiveSettingsFrameThreadsCompatible(202, false, 202));
}

void TestDefaultSettingsValues()
{
    FO4HDR_CHECK(kMaximumSelectiveExposureEv == 2.0);
    const HdrSettings defaults{};
    FO4HDR_CHECK(ValidateHdrSettings(defaults) ==
        HdrSettingsValidationError::none);
    FO4HDR_CHECK(defaults.productionAlgorithm ==
        ProductionToneMapperAlgorithm::gt7);
    FO4HDR_CHECK(defaults.toneMapperParameters == ToneMapperParameters{});
    FO4HDR_CHECK(defaults.toneMapperParameters.acesLookStrength == 1.0);
    FO4HDR_CHECK(defaults.toneMapperParameters.agxMiddleGrayEv == 0.0);
    FO4HDR_CHECK(defaults.toneMapperParameters.agxContrast == 2.7);
    FO4HDR_CHECK(defaults.toneMapperParameters.agxHdrPurity == 0.5);
    FO4HDR_CHECK(defaults.toneMapperParameters.agxHdrShoulderPower == 2.0);
    FO4HDR_CHECK(defaults.toneMapperParameters.gt7ToeStrength == 1.28);
    FO4HDR_CHECK(defaults.toneMapperParameters.gt7ShoulderStart == 0.444);
    FO4HDR_CHECK(defaults.toneMapperParameters.gt7ColorBlend == 0.6);
    FO4HDR_CHECK(defaults.vanillaGradeLuminancePercent == 67.0);
    FO4HDR_CHECK(defaults.vanillaGradeColorPercent == 67.0);
    FO4HDR_CHECK(defaults.bloomStrengthPercent == 50.0);
}

// The SDR-only switch: by default an SDR operator turns the weather response on
// while Native does not.
void CheckSdrOperatorSwitches(const HdrSettings& sdr)
{
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2,
             ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7})
    {
        auto selected = sdr;
        selected.productionAlgorithm = algorithm;
        FO4HDR_CHECK(EffectiveWeatherGradeResponse(selected).enabled);
        FO4HDR_CHECK(!selected.weatherGradeResponse.enabled);
        FO4HDR_CHECK(!SdrGradeControlsNeutral(selected));
    }
    FO4HDR_CHECK(!EffectiveWeatherGradeResponse(sdr).enabled);
    // The HDR profile keeps its own weather setting.
    auto hdr = HdrSettings{};
    hdr.weatherGradeResponse.enabled = false;
    FO4HDR_CHECK(!EffectiveWeatherGradeResponse(hdr).enabled);
}

void TestSdrProfileDefaults()
{
    const auto sdr = DefaultSdrSettings();
    FO4HDR_CHECK(sdr.productionAlgorithm == ProductionToneMapperAlgorithm::native);
    FO4HDR_CHECK(ValidateProfileSettings(sdr, true) == HdrSettingsValidationError::none);
    // The native curve belongs to the SDR profile; operators join it once selectable.
    FO4HDR_CHECK(ValidateHdrSettings(sdr) == HdrSettingsValidationError::unsupportedAlgorithm);
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2,
             ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7})
    {
        auto selected = sdr;
        selected.productionAlgorithm = algorithm;
        FO4HDR_CHECK(ValidateProfileSettings(selected, true) ==
            (IsSelectableSdrToneMapper(algorithm) ? HdrSettingsValidationError::none :
                HdrSettingsValidationError::unsupportedAlgorithm));
        // An SDR profile that selects an operator keeps its scene controls.
        selected.exposureEv = 1.0;
        FO4HDR_CHECK(DeriveHdrSettingsMath(selected).sceneExposureMultiplier == 2.0);
    }
    auto brighter = sdr;
    brighter.exposureEv = 1.0;
    FO4HDR_CHECK(DeriveHdrSettingsMath(brighter).sceneExposureMultiplier == 2.0);
    FO4HDR_CHECK(sdr.bloomStrengthPercent == 100.0);
    FO4HDR_CHECK(sdr.vanillaGradeLuminancePercent == 100.0);
    FO4HDR_CHECK(sdr.vanillaGradeColorPercent == 100.0);
    FO4HDR_CHECK(!sdr.weatherGradeResponse.enabled);
    FO4HDR_CHECK(SdrSceneControlsNeutral(sdr) && SdrGradeControlsNeutral(sdr));
    // The HDR defaults change the vanilla image, so SDR keeps its own profile.
    FO4HDR_CHECK(!SdrSceneControlsNeutral(HdrSettings{}));
    FO4HDR_CHECK(!SdrGradeControlsNeutral(HdrSettings{}));
    FO4HDR_CHECK(HdrSettingsProfiles{}.sdr == sdr);

    using Edit = void (*)(HdrSettings&);
    const auto sceneOnly = [&sdr](Edit edit) {
        auto settings = sdr;
        edit(settings);
        return !SdrSceneControlsNeutral(settings) && SdrGradeControlsNeutral(settings);
    };
    const auto gradeOnly = [&sdr](Edit edit) {
        auto settings = sdr;
        edit(settings);
        return SdrSceneControlsNeutral(settings) && !SdrGradeControlsNeutral(settings);
    };
    for (const Edit edit : {Edit{[](HdrSettings& s) { s.exposureEv = 0.1; }},
             Edit{[](HdrSettings& s) {
                 s.productionAlgorithm = ProductionToneMapperAlgorithm::agX;
                 s.sdrOperatorWeatherResponse = false;
             }},
             Edit{[](HdrSettings& s) { s.bloomStrengthPercent = 99.0; }},
             Edit{[](HdrSettings& s) { s.contrast = 1.0; }},
             Edit{[](HdrSettings& s) { s.blacks = -1.0; }},
             Edit{[](HdrSettings& s) { s.highlights = 1.0; }},
             Edit{[](HdrSettings& s) { s.shadows = 1.0; }},
             Edit{[](HdrSettings& s) { s.saturationPercent = 101.0; }},
             Edit{[](HdrSettings& s) { s.temperature = 1.0; }},
             Edit{[](HdrSettings& s) { s.tint = -1.0; }},
             Edit{[](HdrSettings& s) {
                 s.selectiveColor.enabled = true;
                 s.selectiveColor.regions[2].brightness = 10.0;
             }}})
        FO4HDR_CHECK(sceneOnly(edit));
    for (const Edit edit : {Edit{[](HdrSettings& s) { s.vanillaColorGradingStrengthPercent = 50.0; }},
             Edit{[](HdrSettings& s) { s.vanillaGradeLuminancePercent = 90.0; }},
             Edit{[](HdrSettings& s) { s.vanillaGradeColorPercent = 0.0; }},
             Edit{[](HdrSettings& s) { s.weatherGradeResponse.enabled = true; }}})
        FO4HDR_CHECK(gradeOnly(edit));
    // Enabled Selective Color with neutral regions has no effect to draw.
    auto selective = sdr;
    selective.selectiveColor.enabled = true;
    FO4HDR_CHECK(SdrSceneControlsNeutral(selective));
    CheckSdrOperatorSwitches(sdr);
}

// SDR operator limits apply even while the operator is not selected.
void TestSdrOperatorLimits()
{
    const auto sdr = DefaultSdrSettings();
    // AgX can only lower middle gray at the SDR white.
    auto raisedGray = sdr;
    raisedGray.toneMapperParameters.agxMiddleGrayEv = 0.1;
    FO4HDR_CHECK(ValidateProfileSettings(raisedGray, true) ==
        HdrSettingsValidationError::toneMapperParametersOutOfRange);
    raisedGray.productionAlgorithm = ProductionToneMapperAlgorithm::gt7;
    raisedGray.toneMapperParameters.gt7ShoulderStart = kMinimumGt7ShoulderStart;
    FO4HDR_CHECK(ValidateHdrSettings(raisedGray) == HdrSettingsValidationError::none);
    // SDR GT7 may start its shoulder where its toe ends, below the HDR minimum.
    FO4HDR_CHECK(sdr.toneMapperParameters == kDefaultSdrToneMapperParameters);
    auto toeEndShoulder = sdr;
    toeEndShoulder.productionAlgorithm = ProductionToneMapperAlgorithm::gt7;
    toeEndShoulder.toneMapperParameters.gt7ShoulderStart = kMinimumSdrGt7ShoulderStart;
    FO4HDR_CHECK(ValidateProfileSettings(toeEndShoulder, true) == HdrSettingsValidationError::none);
    FO4HDR_CHECK(ValidateHdrSettings(toeEndShoulder) ==
        HdrSettingsValidationError::toneMapperParametersOutOfRange);
    toeEndShoulder.toneMapperParameters.gt7ShoulderStart = kMinimumSdrGt7ShoulderStart - 0.001;
    FO4HDR_CHECK(ValidateProfileSettings(toeEndShoulder, true) ==
        HdrSettingsValidationError::toneMapperParametersOutOfRange);
}

void TestSettingsEquality()
{
    const HdrSettings defaults{};
    auto settingsCopy = defaults;
    FO4HDR_CHECK(settingsCopy == defaults);
    settingsCopy.selectiveColor.regions[0].hueDegrees = 1.0;
    FO4HDR_CHECK(settingsCopy != defaults);
    settingsCopy.selectiveColor = defaults.selectiveColor;
    FO4HDR_CHECK(settingsCopy == defaults);
    settingsCopy.weatherGradeResponse.color.gain = 0.5;
    FO4HDR_CHECK(settingsCopy != defaults);
    settingsCopy.weatherGradeResponse = defaults.weatherGradeResponse;
    FO4HDR_CHECK(settingsCopy == defaults);

    const HdrInterfaceSettings interfaceDefaults{};
    auto interfaceCopy = interfaceDefaults;
    FO4HDR_CHECK(interfaceCopy == interfaceDefaults);
    interfaceCopy.windowX = 100.0;
    FO4HDR_CHECK(interfaceCopy != interfaceDefaults);
    interfaceCopy = interfaceDefaults;
    interfaceCopy.toggleKey = HdrSettingsHotkey{VK_F10, 0U};
    FO4HDR_CHECK(interfaceCopy != interfaceDefaults);
    interfaceCopy = interfaceDefaults;
    FO4HDR_CHECK(interfaceCopy == interfaceDefaults);

    const PluginConfiguration configurationDefaults{};
    auto configurationCopy = configurationDefaults;
    FO4HDR_CHECK(configurationCopy == configurationDefaults);
    configurationCopy.selectiveColor.regions[0].hueDegrees = 1.0;
    FO4HDR_CHECK(configurationCopy != configurationDefaults);
    configurationCopy = configurationDefaults;
    FO4HDR_CHECK(configurationCopy == configurationDefaults);
    configurationCopy.interfaceSettings.windowY = 100.0;
    FO4HDR_CHECK(configurationCopy != configurationDefaults);
    configurationCopy = configurationDefaults;
    configurationCopy.developerMode = true;
    FO4HDR_CHECK(configurationCopy != configurationDefaults);
    configurationCopy = configurationDefaults;
    FO4HDR_CHECK(configurationCopy == configurationDefaults);

    const auto equalityNan = std::numeric_limits<double>::quiet_NaN();
    auto nanSettingsLeft = defaults;
    auto nanSettingsRight = defaults;
    nanSettingsLeft.exposureEv = equalityNan;
    nanSettingsRight.exposureEv = equalityNan;
    FO4HDR_CHECK(nanSettingsLeft != nanSettingsRight);
    auto nanConfigurationLeft = configurationDefaults;
    auto nanConfigurationRight = configurationDefaults;
    nanConfigurationLeft.sceneExposureMultiplier = equalityNan;
    nanConfigurationRight.sceneExposureMultiplier = equalityNan;
    FO4HDR_CHECK(nanConfigurationLeft != nanConfigurationRight);
}

void TestActiveOperatorParameters()
{
    ToneMapperParameters parameters{};
    parameters.acesLookStrength = 0.25;
    parameters.agxMiddleGrayEv = 0.25;
    parameters.agxContrast = 3.0;
    parameters.agxHdrPurity = 0.6;
    parameters.agxHdrShoulderPower = 2.1;
    parameters.gt7ToeStrength = 1.5;
    parameters.gt7ShoulderStart = 0.55;
    parameters.gt7ColorBlend = 0.25;
    auto active = ActiveToneMapperParameters(
        ProductionToneMapperAlgorithm::aces2, parameters);
    FO4HDR_CHECK(active.acesLookStrength == 0.25);
    FO4HDR_CHECK(active.agxContrast == ToneMapperParameters{}.agxContrast);
    FO4HDR_CHECK(active.gt7ToeStrength ==
        ToneMapperParameters{}.gt7ToeStrength);
    active = ActiveToneMapperParameters(
        ProductionToneMapperAlgorithm::agX, parameters);
    FO4HDR_CHECK(active.acesLookStrength ==
        ToneMapperParameters{}.acesLookStrength);
    FO4HDR_CHECK(active.agxMiddleGrayEv == 0.25);
    FO4HDR_CHECK(active.agxContrast == 3.0);
    FO4HDR_CHECK(active.agxHdrPurity == 0.6);
    FO4HDR_CHECK(active.agxHdrShoulderPower == 2.1);
    FO4HDR_CHECK(active.gt7ColorBlend ==
        ToneMapperParameters{}.gt7ColorBlend);
    active = ActiveToneMapperParameters(
        ProductionToneMapperAlgorithm::gt7, parameters);
    FO4HDR_CHECK(active.acesLookStrength ==
        ToneMapperParameters{}.acesLookStrength);
    FO4HDR_CHECK(active.agxContrast == ToneMapperParameters{}.agxContrast);
    FO4HDR_CHECK(active.gt7ToeStrength == 1.5);
    FO4HDR_CHECK(active.gt7ShoulderStart == 0.55);
    FO4HDR_CHECK(active.gt7ColorBlend == 0.25);
    FO4HDR_CHECK(ActiveToneMapperParameters(
        static_cast<ProductionToneMapperAlgorithm>(0xFF), parameters) ==
        ToneMapperParameters{});
}

void TestDefaultSettingsMath()
{
    const HdrSettings defaults{};
    FO4HDR_CHECK(defaults.colorGradingMethod == ColorGradingMethod::rgbRatio);
    FO4HDR_CHECK(defaults.colorGradingCurve == ColorGradingCurve::gamma22);
    FO4HDR_CHECK(defaults.displayPeakNits == 1000.0);
    FO4HDR_CHECK(defaults.uiPaperWhiteNits == 203.0);
    FO4HDR_CHECK(defaults.videoWhiteNits == 203.0);
    FO4HDR_CHECK(defaults.exposureEv == 0.0);
    FO4HDR_CHECK(defaults.blacks == 0.0);
    const auto derived = DeriveHdrSettingsMath(defaults);
    FO4HDR_CHECK(test::Approx(
        derived.sceneExposureMultiplier, 1.0, 1.0e-15));
    FO4HDR_CHECK(derived.contrastSlope == 1.0);
    FO4HDR_CHECK(derived.blacksStrength == 0.0);
    FO4HDR_CHECK(derived.highlightsEv == 0.0);
    FO4HDR_CHECK(derived.shadowsEv == 0.0);
    FO4HDR_CHECK(derived.saturationScale == 1.0);
    FO4HDR_CHECK(derived.vanillaColorGradingBlend == 1.0);
    FO4HDR_CHECK(test::Approx(derived.vanillaGradeLuminanceRemoval, 0.33, 1.0e-15));
    FO4HDR_CHECK(test::Approx(derived.vanillaGradeColorRemoval, 0.33, 1.0e-15));
    FO4HDR_CHECK(derived.bloomMultiplier == 0.5);
    constexpr std::array<double, 9> identity{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
    FO4HDR_CHECK(derived.colorBalanceBt709 == identity);
}

void TestAdjustedSettingsMath()
{
    HdrSettings adjusted{};
    adjusted.exposureEv = 4.0;
    adjusted.contrast = 100.0;
    adjusted.blacks = 100.0;
    adjusted.highlights = 50.0;
    adjusted.shadows = -50.0;
    adjusted.saturationPercent = 0.0;
    adjusted.vanillaColorGradingStrengthPercent = 25.0;
    adjusted.vanillaGradeLuminancePercent = 25.0;
    adjusted.vanillaGradeColorPercent = 75.0;
    adjusted.bloomStrengthPercent = 50.0;
    const auto adjustedMath = DeriveHdrSettingsMath(adjusted);
    FO4HDR_CHECK(adjustedMath.sceneExposureMultiplier == 16.0);
    FO4HDR_CHECK(adjustedMath.contrastSlope == 2.0);
    FO4HDR_CHECK(adjustedMath.blacksStrength == 0.75);
    FO4HDR_CHECK(adjustedMath.highlightsEv == 1.0);
    FO4HDR_CHECK(adjustedMath.shadowsEv == -1.0);
    FO4HDR_CHECK(adjustedMath.saturationScale == 0.0);
    FO4HDR_CHECK(adjustedMath.vanillaColorGradingBlend == 0.25);
    FO4HDR_CHECK(adjustedMath.vanillaGradeLuminanceRemoval == 0.75);
    FO4HDR_CHECK(adjustedMath.vanillaGradeColorRemoval == 0.25);
    FO4HDR_CHECK(adjustedMath.bloomMultiplier == 0.5);
    for (const auto strength : {0.0, 100.0})
    {
        HdrSettings bloomOnly{};
        bloomOnly.bloomStrengthPercent = strength;
        FO4HDR_CHECK(ValidateHdrSettings(bloomOnly) == HdrSettingsValidationError::none);
        FO4HDR_CHECK(DeriveHdrSettingsMath(bloomOnly).bloomMultiplier == strength / 100.0);
    }
    for (const auto blacks : {-100.0, 100.0})
    {
        HdrSettings blacksOnly{};
        blacksOnly.blacks = blacks;
        FO4HDR_CHECK(ValidateHdrSettings(blacksOnly) ==
            HdrSettingsValidationError::none);
        FO4HDR_CHECK(DeriveHdrSettingsMath(blacksOnly).blacksStrength ==
            blacks * 0.0075);
    }
}

void TestColorBalanceMath()
{
    HdrSettings colorAdjusted{};
    colorAdjusted.temperature = 20.0;
    colorAdjusted.tint = -30.0;
    const auto colorMatrix =
        DeriveHdrSettingsMath(colorAdjusted).colorBalanceBt709;
    constexpr std::array<double, 9> expectedColorMatrix{
        1.0212627931544651, 0.027566926935249494,
            0.0064129259361357938,
        -0.0012828855962777597, 0.99650280298776606,
            0.0026329634878390767,
        -0.0031038820888138231, -0.011582569944983787,
            0.87324218699067924};
    for (std::size_t index = 0; index < colorMatrix.size(); ++index)
        FO4HDR_CHECK(test::Approx(
            colorMatrix[index], expectedColorMatrix[index], 1.0e-12));
}

void TestSettingsDefaultsAndMath()
{
    TestLiveSettingsThreads();
    TestDefaultSettingsValues();
    TestSdrProfileDefaults();
    TestSdrOperatorLimits();
    TestSettingsEquality();
    TestActiveOperatorParameters();
    TestDefaultSettingsMath();
    TestAdjustedSettingsMath();
    TestColorBalanceMath();
}

void TestToneMapperParameterRanges()
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    struct ParameterRange
    {
        ProductionToneMapperAlgorithm algorithm;
        double ToneMapperParameters::*member;
        double minimum;
        double maximum;
    };
    constexpr std::array parameterRanges{
        ParameterRange{ProductionToneMapperAlgorithm::aces2,
            &ToneMapperParameters::acesLookStrength,
            kMinimumAcesLookStrength, kMaximumAcesLookStrength},
        ParameterRange{ProductionToneMapperAlgorithm::agX,
            &ToneMapperParameters::agxMiddleGrayEv,
            kMinimumAgxMiddleGrayEv, kMaximumAgxMiddleGrayEv},
        ParameterRange{ProductionToneMapperAlgorithm::agX,
            &ToneMapperParameters::agxContrast,
            kMinimumAgxContrast, kMaximumAgxContrast},
        ParameterRange{ProductionToneMapperAlgorithm::agX,
            &ToneMapperParameters::agxHdrPurity,
            kMinimumAgxHdrPurity, kMaximumAgxHdrPurity},
        ParameterRange{ProductionToneMapperAlgorithm::agX,
            &ToneMapperParameters::agxHdrShoulderPower,
            kMinimumAgxHdrShoulderPower, kMaximumAgxHdrShoulderPower},
        ParameterRange{ProductionToneMapperAlgorithm::gt7,
            &ToneMapperParameters::gt7ToeStrength,
            kMinimumGt7ToeStrength, kMaximumGt7ToeStrength},
        ParameterRange{ProductionToneMapperAlgorithm::gt7,
            &ToneMapperParameters::gt7ShoulderStart,
            kMinimumGt7ShoulderStart, kMaximumGt7ShoulderStart},
        ParameterRange{ProductionToneMapperAlgorithm::gt7,
            &ToneMapperParameters::gt7ColorBlend,
            kMinimumGt7ColorBlend, kMaximumGt7ColorBlend}};
    for (const auto& range : parameterRanges)
    {
        ToneMapperParameters parameters{};
        parameters.*range.member = range.minimum;
        FO4HDR_CHECK(ValidateToneMapperParameters(
            range.algorithm, parameters));
        parameters.*range.member = range.maximum;
        FO4HDR_CHECK(ValidateToneMapperParameters(
            range.algorithm, parameters));
        for (const auto invalid : {
                 range.minimum - 0.01, range.maximum + 0.01, nan})
        {
            parameters = {};
            parameters.*range.member = invalid;
            FO4HDR_CHECK(!ValidateToneMapperParameters(
                range.algorithm, parameters));
            HdrSettings invalidSettings{};
            invalidSettings.toneMapperParameters = parameters;
            FO4HDR_CHECK(ValidateHdrSettings(invalidSettings) ==
                HdrSettingsValidationError::toneMapperParametersOutOfRange);
        }
    }
    FO4HDR_CHECK(!ValidateToneMapperParameters(
        static_cast<ProductionToneMapperAlgorithm>(0xFF), {}));
}

void TestColorBalanceEndpoints()
{
    for (const auto pair : {
             std::pair{-100.0, -100.0},
             std::pair{100.0, 100.0},
             std::pair{-100.0, 100.0},
             std::pair{100.0, -100.0}})
    {
        HdrSettings value{};
        value.temperature = pair.first;
        value.tint = pair.second;
        const auto matrix = DeriveHdrSettingsMath(value).colorBalanceBt709;
        bool differsFromIdentity{};
        for (std::size_t index = 0; index < matrix.size(); ++index)
        {
            FO4HDR_CHECK(std::isfinite(matrix[index]));
            const auto identity = index == 0 || index == 4 || index == 8 ?
                1.0 : 0.0;
            differsFromIdentity = differsFromIdentity ||
                std::abs(matrix[index] - identity) > 1.0e-6;
        }
        FO4HDR_CHECK(differsFromIdentity);
    }
}

void TestColorGradingValidation()
{
    HdrSettings value{};
    value.colorGradingMethod = static_cast<ColorGradingMethod>(0xFF);
    FO4HDR_CHECK(ValidateHdrSettings(value) ==
        HdrSettingsValidationError::unsupportedColorGradingMethod);
    value.colorGradingMethod = ColorGradingMethod::affine;
    FO4HDR_CHECK(ValidateHdrSettings(value) == HdrSettingsValidationError::none);
    value.colorGradingCurve = static_cast<ColorGradingCurve>(0xFF);
    FO4HDR_CHECK(ValidateHdrSettings(value) ==
        HdrSettingsValidationError::unsupportedColorGradingCurve);
    value.colorGradingCurve = ColorGradingCurve::srgb;
    FO4HDR_CHECK(ValidateHdrSettings(value) == HdrSettingsValidationError::none);
}

void TestSettingsValidation()
{
    HdrSettings value{};
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto check = [&](auto member,
                           double invalid,
                           HdrSettingsValidationError expected)
    {
        auto candidate = value;
        candidate.*member = invalid;
        FO4HDR_CHECK(ValidateHdrSettings(candidate) == expected);
    };

    value.productionAlgorithm =
        static_cast<ProductionToneMapperAlgorithm>(0xFF);
    FO4HDR_CHECK(ValidateHdrSettings(value) ==
        HdrSettingsValidationError::unsupportedAlgorithm);
    value = {};
    value.productionAlgorithm = ProductionToneMapperAlgorithm::aces2;
    FO4HDR_CHECK(ValidateHdrSettings(value) ==
        HdrSettingsValidationError::none);
    TestToneMapperParameterRanges();
    TestColorGradingValidation();
    value = {};
    check(&HdrSettings::displayPeakNits, 399.0,
        HdrSettingsValidationError::displayPeakOutOfRange);
    check(&HdrSettings::uiPaperWhiteNits, 301.0,
        HdrSettingsValidationError::uiPaperWhiteOutOfRange);
    check(&HdrSettings::videoWhiteNits, 99.0,
        HdrSettingsValidationError::videoWhiteOutOfRange);
    check(&HdrSettings::exposureEv, nan,
        HdrSettingsValidationError::exposureOutOfRange);
    check(&HdrSettings::contrast, 100.01,
        HdrSettingsValidationError::contrastOutOfRange);
    check(&HdrSettings::blacks, -100.01,
        HdrSettingsValidationError::blacksOutOfRange);
    check(&HdrSettings::blacks, nan,
        HdrSettingsValidationError::blacksOutOfRange);
    check(&HdrSettings::blacks,
        std::numeric_limits<double>::infinity(),
        HdrSettingsValidationError::blacksOutOfRange);
    check(&HdrSettings::highlights, -100.01,
        HdrSettingsValidationError::highlightsOutOfRange);
    check(&HdrSettings::shadows, 100.01,
        HdrSettingsValidationError::shadowsOutOfRange);
    check(&HdrSettings::saturationPercent, 200.01,
        HdrSettingsValidationError::saturationOutOfRange);
    check(&HdrSettings::temperature, -100.01,
        HdrSettingsValidationError::temperatureOutOfRange);
    check(&HdrSettings::tint, 100.01,
        HdrSettingsValidationError::tintOutOfRange);
    check(&HdrSettings::vanillaColorGradingStrengthPercent, 100.01,
        HdrSettingsValidationError::vanillaColorGradingOutOfRange);
    check(&HdrSettings::vanillaGradeLuminancePercent, -0.01,
        HdrSettingsValidationError::vanillaColorGradingOutOfRange);
    check(&HdrSettings::vanillaGradeColorPercent, nan,
        HdrSettingsValidationError::vanillaColorGradingOutOfRange);
    check(&HdrSettings::bloomStrengthPercent, -0.01,
        HdrSettingsValidationError::bloomStrengthOutOfRange);
    check(&HdrSettings::bloomStrengthPercent, 100.01,
        HdrSettingsValidationError::bloomStrengthOutOfRange);
    TestColorBalanceEndpoints();
}

}  // namespace fo4hdr::tests
