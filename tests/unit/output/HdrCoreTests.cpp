// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"

#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/output/HdrActivationPolicy.hpp"
#include "resources/HdrResourcePolicy.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace fo4hdr::tests {
namespace {

void TestBt709ToBt2020()
{
    const auto neutral = ConvertBt709ToBt2020({4.0, 4.0, 4.0});
    FO4HDR_CHECK(test::Approx(neutral.red, 4.0, 1.0e-7));
    FO4HDR_CHECK(test::Approx(neutral.green, 4.0, 1.0e-7));
    FO4HDR_CHECK(test::Approx(neutral.blue, 4.0, 1.0e-7));

    const auto red = ConvertBt709ToBt2020({1.0, 0.0, 0.0});
    FO4HDR_CHECK(test::Approx(red.red, 0.6274039149284363));
    FO4HDR_CHECK(test::Approx(red.green, 0.06909728795289993));
    FO4HDR_CHECK(test::Approx(red.blue, 0.0163914393633604));

    const auto green = ConvertBt709ToBt2020({0.0, 1.0, 0.0});
    FO4HDR_CHECK(test::Approx(green.red, 0.3292830288410187));
    FO4HDR_CHECK(test::Approx(green.green, 0.9195404052734375));
    FO4HDR_CHECK(test::Approx(green.blue, 0.08801330626010895));

    const auto blue = ConvertBt709ToBt2020({0.0, 0.0, 1.0});
    FO4HDR_CHECK(test::Approx(blue.red, 0.04331306740641594));
    FO4HDR_CHECK(test::Approx(blue.green, 0.01136231515556574));
    FO4HDR_CHECK(test::Approx(blue.blue, 0.8955952525138855));

    const auto emissive = ConvertBt709ToBt2020({8.0, 4.0, 0.0});
    FO4HDR_CHECK(emissive.red > 1.0);
    FO4HDR_CHECK(emissive.green > 1.0);
    FO4HDR_CHECK(emissive.blue > 0.0);

    for (const auto sample : {
             LinearRgb{1.0, 0.0, 0.0}, LinearRgb{0.0, 1.0, 0.0},
             LinearRgb{0.0, 0.0, 1.0}, LinearRgb{8.0, 4.0, 0.5},
             LinearRgb{203.0, 203.0, 203.0}})
    {
        const auto roundTrip = ConvertBt2020ToBt709ForComparison(
            ConvertBt709ToBt2020(sample));
        FO4HDR_CHECK(test::Approx(roundTrip.red, sample.red, 1.0e-12));
        FO4HDR_CHECK(test::Approx(roundTrip.green, sample.green, 1.0e-12));
        FO4HDR_CHECK(test::Approx(roundTrip.blue, sample.blue, 1.0e-12));
    }
}

void TestDisplayStandardsOracle()
{
    // Independent XYZ route from ITU primary coordinates and D65, without
    // rounding the combined RGB transform to the shader's binary32 precision.
    // https://www.itu.int/rec/R-REC-BT.709-6-201506-I/en
    // https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en
    using Matrix = std::array<std::array<double, 3>, 3>;
    constexpr Matrix narrowToXyz{{
        {0.4123907992659595, 0.3575843393838780, 0.1804807884018343},
        {0.2126390058715104, 0.7151686787677560, 0.0721923153607337},
        {0.0193308187155919, 0.1191947797946260, 0.9505321522496607}
    }};
    constexpr Matrix wideToXyz{{
        {0.6369580483012914, 0.1446169035862083, 0.1688809751641721},
        {0.2627002120112671, 0.6779980715188710, 0.0593017164698620},
        {0.0, 0.0280726930490874, 1.0609850577107910}
    }};
    for (const auto input : {
        LinearRgb{1, 0, 0}, LinearRgb{0, 1, 0}, LinearRgb{0, 0, 1},
        LinearRgb{203, 203, 203}, LinearRgb{-3, 8, -0.25},
        LinearRgb{65504, 0.0001, 32752}})
    {
        const auto output = ConvertBt709ToBt2020(input);
        for (std::size_t row = 0; row < 3; ++row) {
            const auto& a = narrowToXyz[row];
            const auto& b = wideToXyz[row];
            const double expected = a[0] * input.red + a[1] * input.green + a[2] * input.blue;
            const double actual = b[0] * output.red + b[1] * output.green + b[2] * output.blue;
            const double scale = std::max({1.0, std::abs(input.red),
                std::abs(input.green), std::abs(input.blue)});
            FO4HDR_CHECK(std::abs(actual - expected) <= scale * 5.0e-8);
        }
    }

}

void TestDisplaySignalContainment()
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto bounded = ContainBt2020Nits({nan, -infinity, infinity}, 400.0);
    FO4HDR_CHECK(bounded.red == 0.0 && bounded.green == 0.0 && bounded.blue == 400.0);
    for (const double invalidPeak : {-1.0, 0.0, 10001.0, infinity, nan}) {
        const auto black = ContainBt2020Nits({100, 200, 300}, invalidPeak);
        FO4HDR_CHECK(black.red == 0.0 && black.green == 0.0 && black.blue == 0.0);
    }
    const auto preserved = ContainBt2020Nits({0.001, 203, 401}, 400);
    FO4HDR_CHECK(preserved.red == 0.001 && preserved.green == 203 && preserved.blue == 400);
    FO4HDR_CHECK(QuantizeR10Unorm(nan) == 0);
    FO4HDR_CHECK(QuantizeR10Unorm(infinity) == 1023);
    FO4HDR_CHECK(NormalizeR10Unorm(65535) == 1.0);
}

void TestSt2084ReferenceVectors()
{
    struct Reference
    {
        double nits;
        double encoded;
        std::uint16_t r10Code;
    };
    constexpr std::array<Reference, 12> references{{
        {0.0, 7.309559025783966e-7, 0},
        {0.001, 0.006302377054571391, 6},
        {0.01, 0.02148621379868528, 22},
        {0.1, 0.06233686566269587, 64},
        {1.0, 0.14994573210018022, 153},
        {80.0, 0.4858567653886785, 497},
        {100.0, 0.508078421517399, 520},
        {203.0, 0.5806888810416109, 594},
        {400.0, 0.6525785975630672, 668},
        {1000.0, 0.751827096247041, 769},
        {2000.0, 0.8274246448588193, 846},
        {10000.0, 1.0, 1023}
    }};

    double previous = -1.0;
    for (const auto& reference : references)
    {
        const auto encoded = EncodeSt2084Pq(reference.nits);
        FO4HDR_CHECK(test::Approx(encoded, reference.encoded, 1.0e-12));
        FO4HDR_CHECK(encoded >= previous);
        FO4HDR_CHECK(encoded >= 0.0 && encoded <= 1.0);
        const auto code = QuantizeR10Unorm(encoded);
        FO4HDR_CHECK(code == reference.r10Code);
        FO4HDR_CHECK(code <= 1023);
        FO4HDR_CHECK(
            std::abs(NormalizeR10Unorm(code) - encoded) <=
            0.5 / 1023.0 + 1.0e-12);
        previous = encoded;
    }

    FO4HDR_CHECK(EncodeSt2084Pq(-1.0) == EncodeSt2084Pq(0.0));
    FO4HDR_CHECK(EncodeSt2084Pq(20000.0) == 1.0);
    FO4HDR_CHECK(
        EncodeSt2084Pq(std::numeric_limits<double>::infinity()) == 1.0);
    FO4HDR_CHECK(
        EncodeSt2084Pq(std::numeric_limits<double>::quiet_NaN()) ==
        EncodeSt2084Pq(0.0));
    FO4HDR_CHECK(QuantizeR10Unorm(-1.0) == 0);
    FO4HDR_CHECK(QuantizeR10Unorm(2.0) == 1023);
}

void TestWideGamutReadinessAndSdrHandoff()
{
    WideGamutResourceReadiness readiness{
        .replacementShaderSet = true,
        .presentationProxySet = true,
        .compositionSet = true,
        .uiSet = true,
        .outputSet = true
    };
    FO4HDR_CHECK(readiness.Complete());
    constexpr std::array partialSets{
        WideGamutResourceReadiness{false, true, true, true, true},
        WideGamutResourceReadiness{true, false, true, true, true},
        WideGamutResourceReadiness{true, true, false, true, true},
        WideGamutResourceReadiness{true, true, true, false, true},
        WideGamutResourceReadiness{true, true, true, true, false}
    };
    for (const auto partial : partialSets)
        FO4HDR_CHECK(!partial.Complete());
    FO4HDR_CHECK(IsWideGamutGenerationReady(readiness, 7, 7));
    FO4HDR_CHECK(!IsWideGamutGenerationReady(readiness, 0, 0));
    FO4HDR_CHECK(!IsWideGamutGenerationReady(readiness, 6, 7));
    FO4HDR_CHECK(!IsWideGamutGenerationReady(partialSets[0], 7, 7));

    FO4HDR_CHECK(DecideSdrHandoff(true, true,
        LinearColorDomain::linearBt2020UiRelative, true) ==
        SdrHandoffAction::none);
    FO4HDR_CHECK(DecideSdrHandoff(false, false,
        LinearColorDomain::unknown, false) == SdrHandoffAction::none);
    FO4HDR_CHECK(DecideSdrHandoff(false, true,
        LinearColorDomain::vanillaSdrCodes, true) ==
        SdrHandoffAction::copyVanillaSdr);
    FO4HDR_CHECK(DecideSdrHandoff(false, true,
        LinearColorDomain::vanillaSdrCodes, false) ==
        SdrHandoffAction::clearBlack);
    FO4HDR_CHECK(DecideSdrHandoff(false, true,
        LinearColorDomain::linearBt709Relative, true) ==
        SdrHandoffAction::clearBlack);
    FO4HDR_CHECK(DecideSdrHandoff(false, true,
        LinearColorDomain::linearBt2020UiRelative, true) ==
        SdrHandoffAction::clearBlack);
    FO4HDR_CHECK(DecideSdrHandoff(false, true,
        LinearColorDomain::unknown, true) ==
        SdrHandoffAction::clearBlackAndDisable);
}

void TestAboveWhiteSurvivesReferencePipeline()
{
    constexpr double referenceWhiteNits = 203.0;
    const auto whiteNits = ConvertBt709ToBt2020(
        {referenceWhiteNits, referenceWhiteNits, referenceWhiteNits});
    const auto highlightNits = ConvertBt709ToBt2020(
        {812.0, 812.0, 812.0});
    const auto whitePq = EncodeSt2084Pq(whiteNits);
    const auto highlightPq = EncodeSt2084Pq(highlightNits);

    FO4HDR_CHECK(highlightNits.red > referenceWhiteNits);
    FO4HDR_CHECK(highlightPq.red > whitePq.red);
    FO4HDR_CHECK(highlightPq.green > whitePq.green);
    FO4HDR_CHECK(highlightPq.blue > whitePq.blue);
    FO4HDR_CHECK(
        QuantizeR10Unorm(highlightPq.red) >
        QuantizeR10Unorm(whitePq.red));
}

void TestTemporalIntermediateRoundTrip()
{
    constexpr std::array samples{
        0.0, 0.001, 0.01, 0.18, 1.0, 2.0, 4.0, 8.0, 16.0,
        65504.0};
    double priorEncoded = -1.0;
    for (const auto sample : samples)
    {
        const auto encoded = EncodeTemporalIntermediate(sample);
        const auto decoded = DecodeTemporalIntermediate(encoded);
        FO4HDR_CHECK(std::isfinite(encoded));
        FO4HDR_CHECK(std::isfinite(decoded));
        FO4HDR_CHECK(encoded >= priorEncoded);
        FO4HDR_CHECK(encoded >= 0.0 && encoded <= 1.0);
        FO4HDR_CHECK(test::Approx(
            decoded, sample, std::max(1.0e-9, sample * 1.0e-9)));
        priorEncoded = encoded;
    }

    FO4HDR_CHECK(EncodeTemporalIntermediate(-1.0) == 0.0);
    FO4HDR_CHECK(DecodeTemporalIntermediate(-1.0) == 0.0);
    FO4HDR_CHECK(EncodeTemporalIntermediate(
        std::numeric_limits<double>::quiet_NaN()) == 0.0);
    FO4HDR_CHECK(DecodeTemporalIntermediate(
        std::numeric_limits<double>::quiet_NaN()) == 0.0);
    FO4HDR_CHECK(DecodeTemporalIntermediate(
        std::numeric_limits<double>::infinity()) == 65504.0);
}

void TestResourcePromotionPredicate()
{
    SceneResourceDescription eligible{
        .format = SceneResourceFormat::r11g11b10Float};
    for (const auto dimensions : std::array{
             std::pair{1920U, 1080U},
             std::pair{1440U, 810U},
             std::pair{960U, 540U}})
    {
        eligible.width = dimensions.first;
        eligible.height = dimensions.second;
        FO4HDR_CHECK(
            ShouldPromoteSceneResourceToFp16(eligible, 1920, 1080));
    }
    eligible.format = SceneResourceFormat::r16g16b16a16Float;
    FO4HDR_CHECK(!ShouldPromoteSceneResourceToFp16(eligible, 1920, 1080));
    eligible.format = SceneResourceFormat::other;
    FO4HDR_CHECK(!ShouldPromoteSceneResourceToFp16(eligible, 1920, 1080));
    eligible.format = SceneResourceFormat::r11g11b10Float;
    eligible.width = 1280;
    eligible.height = 800;
    FO4HDR_CHECK(!ShouldPromoteSceneResourceToFp16(eligible, 1920, 1080));
    eligible.width = 0;
    FO4HDR_CHECK(!ShouldPromoteSceneResourceToFp16(eligible, 1920, 1080));
}

HdrPathCapabilities AllCapabilities()
{
    return {
        .runtimeAuthorized = true,
        .signalContractReady = true,
        .fp16WorkingImage = true,
        .r10g10b10a2Presentation = true,
        .flipModel = true,
        .bufferCount = 2,
        .pqBt2020PresentSupported = true,
        .pqBt2020Applied = true,
        .resourcesReady = true,
        .productionToneMapperReady = false
    };
}

void TestActivationPolicy()
{
    auto capabilities = AllCapabilities();
    HdrActivationRequest developer{
        .enabled = true,
        .developerMode = true,
        .developerReferenceRequested = true
    };
    auto decision = DecideHdrActivation(developer, capabilities);
    FO4HDR_CHECK(decision.hdrActive);
    FO4HDR_CHECK(!decision.production);
    FO4HDR_CHECK(
        decision.mode == HdrActivationMode::developerReferenceHdr10);

    developer.developerMode = false;
    decision = DecideHdrActivation(developer, capabilities);
    FO4HDR_CHECK(!decision.hdrActive);
    FO4HDR_CHECK(
        decision.reason == HdrActivationReason::developerModeRequired);

    HdrActivationRequest production{
        .enabled = true,
        .productionRequested = true
    };
    decision = DecideHdrActivation(production, capabilities);
    FO4HDR_CHECK(!decision.hdrActive);
    FO4HDR_CHECK(
        decision.reason ==
        HdrActivationReason::productionToneMapperUnavailable);
    capabilities.productionToneMapperReady = true;
    decision = DecideHdrActivation(production, capabilities);
    FO4HDR_CHECK(decision.hdrActive);
    FO4HDR_CHECK(decision.production);
}

void TestActivationCapabilityRequirements()
{
    struct Requirement
    {
        bool HdrPathCapabilities::*available;
        HdrActivationReason reason;
    };
    constexpr std::array requirements{
        Requirement{&HdrPathCapabilities::pqBt2020PresentSupported,
            HdrActivationReason::colorSpaceUnsupported},
        Requirement{&HdrPathCapabilities::pqBt2020Applied,
            HdrActivationReason::colorSpaceApplicationFailed},
        Requirement{&HdrPathCapabilities::r10g10b10a2Presentation,
            HdrActivationReason::presentationFormatUnavailable},
        Requirement{&HdrPathCapabilities::fp16WorkingImage,
            HdrActivationReason::workingFormatUnavailable},
        Requirement{&HdrPathCapabilities::signalContractReady,
            HdrActivationReason::signalContractNotReady},
        Requirement{&HdrPathCapabilities::runtimeAuthorized,
            HdrActivationReason::runtimeUnauthorized},
        Requirement{&HdrPathCapabilities::flipModel,
            HdrActivationReason::flipModelRequired},
        Requirement{&HdrPathCapabilities::resourcesReady,
            HdrActivationReason::resourcesUnavailable}
    };
    constexpr HdrActivationRequest request{
        .enabled = true,
        .developerMode = true,
        .developerReferenceRequested = true
    };
    for (const auto& requirement : requirements) {
        auto capabilities = AllCapabilities();
        capabilities.*(requirement.available) = false;
        const auto decision = DecideHdrActivation(request, capabilities);
        FO4HDR_CHECK(!decision.hdrActive);
        FO4HDR_CHECK(decision.reason == requirement.reason);
    }

    auto capabilities = AllCapabilities();
    capabilities.bufferCount = 1;
    const auto decision = DecideHdrActivation(request, capabilities);
    FO4HDR_CHECK(
        decision.reason == HdrActivationReason::insufficientBufferCount);
}

void TestHeartbeatFrameQualification(PipelineHeartbeat& heartbeat)
{
    heartbeat.BeginFrame(1, 2);
    heartbeat.Observe(RenoDxShaderPass::toneMapNewB);
    heartbeat.Observe(RenoDxShaderPass::lut);
    const auto evaluation = heartbeat.Evaluate(1, 2);
    FO4HDR_CHECK(evaluation.ready);
    FO4HDR_CHECK(evaluation.ordered);
    FO4HDR_CHECK(
        evaluation.outcome == PipelineHeartbeatEvaluation::Outcome::ready);
    FO4HDR_CHECK(
        DecidePipelineHeartbeatAction(evaluation) ==
        PipelineHeartbeatAction::renderHdr);
    FO4HDR_CHECK(
        DecideTemporalSceneAction(evaluation, {false, true, true, false}) ==
        TemporalSceneAction::ignoreUnqualifiedBranch);
    FO4HDR_CHECK(
        DecideTemporalSceneAction(evaluation, {true, false, true, false}) ==
        TemporalSceneAction::ignoreUnqualifiedBranch);
    FO4HDR_CHECK(
        DecideTemporalSceneAction(evaluation, {true, true, false, false}) ==
        TemporalSceneAction::ignoreUnqualifiedBranch);
    FO4HDR_CHECK(
        DecideTemporalSceneAction(evaluation, {true, true, true, false}) ==
        TemporalSceneAction::rejectMissingProxy);
    FO4HDR_CHECK(
        DecideTemporalSceneAction(evaluation, {true, true, true, true}) ==
        TemporalSceneAction::acceptProxy);
    FO4HDR_CHECK(
        heartbeat.Evaluate(2, 2).outcome ==
        PipelineHeartbeatEvaluation::Outcome::staleGeneration);
    FO4HDR_CHECK(
        DecidePipelineHeartbeatAction(heartbeat.Evaluate(2, 2)) ==
        PipelineHeartbeatAction::disableGeneration);
    FO4HDR_CHECK(
        heartbeat.Evaluate(1, 3).outcome ==
        PipelineHeartbeatEvaluation::Outcome::staleFrame);
}

void TestOrderedHeartbeat()
{
    PipelineHeartbeat heartbeat;
    heartbeat.Reset(1);
    heartbeat.BeginFrame(1, 1);
    auto evaluation = heartbeat.Evaluate(1, 1);
    FO4HDR_CHECK(
        DecidePipelineHeartbeatAction(evaluation) ==
        PipelineHeartbeatAction::waitInSdr);
    heartbeat.Observe(RenoDxShaderPass::taa);
    evaluation = heartbeat.Evaluate(1, 1);
    FO4HDR_CHECK(
        DecideTemporalSceneAction(evaluation, {true, true, true, false}) ==
        TemporalSceneAction::ignoreUnqualifiedBranch);
    heartbeat.Observe(RenoDxShaderPass::lut);
    evaluation = heartbeat.Evaluate(1, 1);
    FO4HDR_CHECK(!evaluation.ready);
    FO4HDR_CHECK(evaluation.lutObserved);
    FO4HDR_CHECK(!evaluation.toneMapFamilyObserved);
    FO4HDR_CHECK(
        evaluation.outcome ==
        PipelineHeartbeatEvaluation::Outcome::incomplete);
    FO4HDR_CHECK(
        DecidePipelineHeartbeatAction(evaluation) ==
        PipelineHeartbeatAction::waitInSdr);
    heartbeat.Observe(RenoDxShaderPass::toneMapA);
    evaluation = heartbeat.Evaluate(1, 1);
    FO4HDR_CHECK(!evaluation.ready);
    FO4HDR_CHECK(!evaluation.ordered);
    FO4HDR_CHECK(
        evaluation.outcome ==
        PipelineHeartbeatEvaluation::Outcome::incomplete);
    FO4HDR_CHECK(
        DecidePipelineHeartbeatAction(evaluation) ==
        PipelineHeartbeatAction::waitInSdr);
    heartbeat.Observe(RenoDxShaderPass::lut);
    evaluation = heartbeat.Evaluate(1, 1);
    FO4HDR_CHECK(evaluation.ready);
    FO4HDR_CHECK(evaluation.ordered);

    TestHeartbeatFrameQualification(heartbeat);

    heartbeat.BeginFrame(1, 3);
    heartbeat.Observe(RenoDxShaderPass::toneMapA);
    heartbeat.Observe(RenoDxShaderPass::toneMapB);
    heartbeat.Observe(RenoDxShaderPass::lut);
    FO4HDR_CHECK(
        heartbeat.Evaluate(1, 3).outcome ==
        PipelineHeartbeatEvaluation::Outcome::ready);

    heartbeat.BeginFrame(1, 4);
    heartbeat.Observe(RenoDxShaderPass::toneMapA);
    heartbeat.Observe(RenoDxShaderPass::lut);
    heartbeat.Observe(RenoDxShaderPass::toneMapB);
    FO4HDR_CHECK(
        heartbeat.Evaluate(1, 4).outcome ==
        PipelineHeartbeatEvaluation::Outcome::ready);
}

void TestLateGameUiCapturePolicy()
{
    FO4HDR_CHECK(DecideLateGameUiAction({true, true, true, true, true, true}, true, true) ==
        LateGameUiAction::captureSeparately);

    constexpr std::array<std::array<bool, 8>, 8> rejected{{
        {{false, true, true, true, true, true, true, true}},
        {{true, false, true, true, true, true, true, true}},
        {{true, true, false, true, true, true, true, true}},
        {{true, true, true, false, true, true, true, true}},
        {{true, true, true, true, false, true, true, true}},
        {{true, true, true, true, true, false, true, true}},
        {{true, true, true, true, true, true, false, true}},
        {{true, true, true, true, true, true, true, false}},
    }};
    for (const auto& value : rejected) {
        FO4HDR_CHECK(DecideLateGameUiAction({value[0], value[1], value[2], value[3], value[4], value[5]}, value[6], value[7]) ==
            LateGameUiAction::passThrough);
    }

    FO4HDR_CHECK(DecideLateGameUiBackdropAction({true, true, true, true, true, true}, {true, true, true, true}) ==
        LateGameUiAction::captureSeparately);
    constexpr std::array<std::array<bool, 10>, 10> backdropRejected{{
        {{false, true, true, true, true, true, true, true, true, true}},
        {{true, false, true, true, true, true, true, true, true, true}},
        {{true, true, false, true, true, true, true, true, true, true}},
        {{true, true, true, false, true, true, true, true, true, true}},
        {{true, true, true, true, false, true, true, true, true, true}},
        {{true, true, true, true, true, false, true, true, true, true}},
        {{true, true, true, true, true, true, false, true, true, true}},
        {{true, true, true, true, true, true, true, false, true, true}},
        {{true, true, true, true, true, true, true, true, false, true}},
        {{true, true, true, true, true, true, true, true, true, false}},
    }};
    for (const auto& value : backdropRejected) {
        FO4HDR_CHECK(DecideLateGameUiBackdropAction({value[0], value[1], value[2], value[3], value[4], value[5]}, {value[6], value[7], value[8], value[9]}) ==
            LateGameUiAction::passThrough);
    }
}

void TestRendererPauseAndSdr(HdrRendererLifecycle& lifecycle)
{
    FO4HDR_CHECK(lifecycle.BindDeviceAndSwapChain());
    FO4HDR_CHECK(lifecycle.PublishHdrResources());
    const auto publishedGeneration =
        lifecycle.Snapshot().resourceGeneration;
    FO4HDR_CHECK(lifecycle.PauseHdrPresentation());
    FO4HDR_CHECK(lifecycle.Snapshot().resourcesPublished);
    FO4HDR_CHECK(!lifecycle.Snapshot().hdrActive);
    FO4HDR_CHECK(
        lifecycle.Snapshot().resourceGeneration == publishedGeneration);
    FO4HDR_CHECK(lifecycle.ResumeHdrPresentation());
    FO4HDR_CHECK(lifecycle.Snapshot().hdrActive);
    FO4HDR_CHECK(lifecycle.DeactivateToSdr());
    FO4HDR_CHECK(!lifecycle.Snapshot().resourcesPublished);
    const auto sdrGeneration = lifecycle.Snapshot().resourceGeneration;
    FO4HDR_CHECK(lifecycle.DeactivateToSdr());
    FO4HDR_CHECK(
        lifecycle.Snapshot().resourceGeneration == sdrGeneration);
    FO4HDR_CHECK(lifecycle.PublishHdrResources());
}

void TestRendererRepeatedResets(HdrRendererLifecycle& lifecycle)
{
    auto prior = lifecycle.Snapshot();
    for (std::uint32_t iteration = 0; iteration < 10000; ++iteration)
    {
        const auto resetEpoch = static_cast<std::uint64_t>(iteration) + 1;
        const auto beforeResetGeneration =
            lifecycle.Snapshot().resourceGeneration;
        FO4HDR_CHECK(lifecycle.OnGameReset(resetEpoch));
        FO4HDR_CHECK(
            lifecycle.Snapshot().gameResetEpoch == resetEpoch);
        FO4HDR_CHECK(lifecycle.Snapshot().resourcesPublished);
        FO4HDR_CHECK(!lifecycle.Snapshot().hdrActive);
        FO4HDR_CHECK(
            lifecycle.Snapshot().resourceGeneration >
            beforeResetGeneration);
        const auto afterResetGeneration =
            lifecycle.Snapshot().resourceGeneration;
        FO4HDR_CHECK(!lifecycle.OnGameReset(resetEpoch));
        FO4HDR_CHECK(
            lifecycle.Snapshot().resourceGeneration ==
            afterResetGeneration);
        const auto oldResourceGeneration =
            lifecycle.Snapshot().resourceGeneration;
        FO4HDR_CHECK(lifecycle.BeginResize());
        FO4HDR_CHECK(!lifecycle.Snapshot().resourcesPublished);
        FO4HDR_CHECK(!lifecycle.Snapshot().hdrActive);
        FO4HDR_CHECK(
            !lifecycle.IsCurrentResourceGeneration(oldResourceGeneration));
        FO4HDR_CHECK(lifecycle.CompleteResize(true, true));
        const auto current = lifecycle.Snapshot();
        FO4HDR_CHECK(current.state == HdrRendererState::hdr10Ready);
        FO4HDR_CHECK(current.resourcesPublished);
        FO4HDR_CHECK(current.hdrActive);
        FO4HDR_CHECK(current.swapChainGeneration > prior.swapChainGeneration);
        FO4HDR_CHECK(current.resourceGeneration > prior.resourceGeneration);
        FO4HDR_CHECK(
            lifecycle.IsCurrentResourceGeneration(
                current.resourceGeneration));
        prior = current;

        lifecycle.OnDisplayChange();
        FO4HDR_CHECK(
            lifecycle.Snapshot().state == HdrRendererState::vanillaSdr);
        FO4HDR_CHECK(!lifecycle.Snapshot().resourcesPublished);
        FO4HDR_CHECK(lifecycle.PublishHdrResources());

        const auto deviceResourceGeneration =
            lifecycle.Snapshot().resourceGeneration;
        lifecycle.OnDeviceLost();
        FO4HDR_CHECK(
            lifecycle.Snapshot().state == HdrRendererState::deviceLost);
        FO4HDR_CHECK(!lifecycle.Snapshot().resourcesPublished);
        FO4HDR_CHECK(
            !lifecycle.IsCurrentResourceGeneration(
                deviceResourceGeneration));
        FO4HDR_CHECK(lifecycle.BindDeviceAndSwapChain());
        FO4HDR_CHECK(lifecycle.PublishHdrResources());
        prior = lifecycle.Snapshot();
    }
}

void TestRendererLifecycleStress()
{
    HdrRendererLifecycle lifecycle;
    TestRendererPauseAndSdr(lifecycle);
    TestRendererRepeatedResets(lifecycle);
    lifecycle.OnDeviceLost();
    FO4HDR_CHECK(lifecycle.Snapshot().state == HdrRendererState::deviceLost);
    FO4HDR_CHECK(!lifecycle.Snapshot().resourcesPublished);
    const auto lostGeneration = lifecycle.Snapshot().resourceGeneration;
    lifecycle.OnDeviceLost();
    FO4HDR_CHECK(
        lifecycle.Snapshot().resourceGeneration == lostGeneration);
    FO4HDR_CHECK(lifecycle.BindDeviceAndSwapChain());
    FO4HDR_CHECK(lifecycle.Snapshot().state == HdrRendererState::vanillaSdr);
    FO4HDR_CHECK(lifecycle.PublishHdrResources());
    FO4HDR_CHECK(lifecycle.BeginResize());
    FO4HDR_CHECK(!lifecycle.CompleteResize(false, false));
    FO4HDR_CHECK(
        lifecycle.Snapshot().state == HdrRendererState::vanillaSdr);
    FO4HDR_CHECK(!lifecycle.Snapshot().resourcesPublished);
    lifecycle.OnDeviceLost();
    FO4HDR_CHECK(lifecycle.Snapshot().state == HdrRendererState::deviceLost);
}

void TestGraphicsLifecyclePublication()
{
    GraphicsLifecyclePublisher publisher;
    auto publication = publisher.Read();
    FO4HDR_CHECK(!publication.gameplayReady);
    FO4HDR_CHECK(!publication.presentationBlocked);
    FO4HDR_CHECK(publication.resetEpoch == 0);

    publication = publisher.Publish(true, false);
    FO4HDR_CHECK(publication.gameplayReady);
    FO4HDR_CHECK(!publication.presentationBlocked);
    FO4HDR_CHECK(publication.resetEpoch == 0);
    const auto unchanged = publisher.Publish(true, false);
    FO4HDR_CHECK(unchanged.gameplayReady);
    FO4HDR_CHECK(unchanged.resetEpoch == 0);

    publication = publisher.Publish(false, true);
    FO4HDR_CHECK(!publication.gameplayReady);
    FO4HDR_CHECK(publication.resetEpoch == 1);
    publication = publisher.Publish(true, false);
    FO4HDR_CHECK(publication.gameplayReady);
    FO4HDR_CHECK(publication.resetEpoch == 1);

    publication = publisher.PublishPresentationBlocked(true);
    FO4HDR_CHECK(publication.gameplayReady);
    FO4HDR_CHECK(publication.presentationBlocked);
    FO4HDR_CHECK(publication.resetEpoch == 1);
    publication = publisher.Publish(false, false);
    FO4HDR_CHECK(!publication.gameplayReady);
    FO4HDR_CHECK(publication.presentationBlocked);
    publication = publisher.PublishPresentationBlocked(false);
    FO4HDR_CHECK(!publication.gameplayReady);
    FO4HDR_CHECK(!publication.presentationBlocked);
    publication = publisher.Publish(true, false);

    for (std::uint64_t reset = 2; reset <= 10001; ++reset)
    {
        // Repeated NewGame-style readiness must still advance the epoch.
        publication = publisher.Publish(true, true);
        FO4HDR_CHECK(publication.gameplayReady);
        FO4HDR_CHECK(!publication.presentationBlocked);
        FO4HDR_CHECK(publication.resetEpoch == reset);
        const auto observed = publisher.Read();
        FO4HDR_CHECK(observed.gameplayReady);
        FO4HDR_CHECK(observed.resetEpoch == reset);
    }
    const auto finalEpoch = publication.resetEpoch;
    publication = publisher.Publish(false, false);
    FO4HDR_CHECK(!publication.gameplayReady);
    FO4HDR_CHECK(publication.resetEpoch == finalEpoch);

    HdrRendererLifecycle retirement;
    FO4HDR_CHECK(retirement.BindDeviceAndSwapChain());
    FO4HDR_CHECK(retirement.PublishHdrResources());
    const auto published = retirement.Snapshot().resourceGeneration;
    retirement.Detach();
    FO4HDR_CHECK(!retirement.Snapshot().resourcesPublished);
    FO4HDR_CHECK(!retirement.IsCurrentResourceGeneration(published));
    const auto detached = retirement.Snapshot().resourceGeneration;
    retirement.Detach();
    FO4HDR_CHECK(retirement.Snapshot().resourceGeneration == detached);

    FO4HDR_CHECK(retirement.BindDeviceAndSwapChain());
    FO4HDR_CHECK(retirement.PublishHdrResources());
    const auto republished = retirement.Snapshot().resourceGeneration;
    retirement.Disable();
    FO4HDR_CHECK(!retirement.Snapshot().resourcesPublished);
    FO4HDR_CHECK(!retirement.IsCurrentResourceGeneration(republished));
    const auto disabled = retirement.Snapshot().resourceGeneration;
    retirement.Disable();
    FO4HDR_CHECK(retirement.Snapshot().resourceGeneration == disabled);
}

void TestDeveloperConfiguration()
{
    constexpr std::string_view enabled = R"ini(
[General]
DeveloperMode=1
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=1
ShowDeveloperOverlay=1
ForceHdrSwapChainFailure=0
)ini";
    auto parsed = ParsePluginConfiguration(enabled);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.developerHdr10Reference);
    FO4HDR_CHECK(parsed.value.showDeveloperOverlay);
    FO4HDR_CHECK(!parsed.value.forceHdrSwapChainFailure);

    constexpr std::string_view overlayWithoutReference = R"ini(
[General]
DeveloperMode=1
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ShowDeveloperOverlay=1
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(overlayWithoutReference);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(
        parsed.error ==
        PluginConfigurationError::invalidCombination);

    constexpr std::string_view referenceWithoutDeveloper = R"ini(
[General]
DeveloperMode=0
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=1
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(referenceWithoutDeveloper);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(
        parsed.error ==
        PluginConfigurationError::invalidCombination);

    constexpr std::string_view failureWithoutReference = R"ini(
[General]
DeveloperMode=1
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=1
)ini";
    parsed = ParsePluginConfiguration(failureWithoutReference);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(
        parsed.error ==
        PluginConfigurationError::invalidCombination);
}

}  // namespace

void RunHdrCoreTests()
{
    TestBt709ToBt2020();
    TestDisplayStandardsOracle();
    TestDisplaySignalContainment();
    TestSt2084ReferenceVectors();
    TestWideGamutReadinessAndSdrHandoff();
    TestAboveWhiteSurvivesReferencePipeline();
    TestTemporalIntermediateRoundTrip();
    TestResourcePromotionPredicate();
    TestActivationPolicy();
    TestActivationCapabilityRequirements();
    TestOrderedHeartbeat();
    TestLateGameUiCapturePolicy();
    TestRendererLifecycleStress();
    TestGraphicsLifecyclePublication();
    TestDeveloperConfiguration();
}

}  // namespace fo4hdr::tests
