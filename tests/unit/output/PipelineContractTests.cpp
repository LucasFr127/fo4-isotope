// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>

namespace fo4hdr::tests {
namespace {

void TestRenoDxCrcDomain()
{
    constexpr std::array<std::byte, 0> empty{};
    FO4HDR_CHECK(ComputeRenoDxShaderCrc32(empty) == 0u);

    constexpr std::string_view standardVector = "123456789";
    std::array<std::byte, standardVector.size()> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>(standardVector[index]);
    }
    FO4HDR_CHECK(
        ComputeRenoDxShaderCrc32(bytes) == 0xCBF43926u);
}

void TestTrustedCatalog()
{
    constexpr std::array<std::uint32_t, 15> expected{{
        0x8024E8B5u,
        0x676B8B5Du,
        0x67685D89u,
        0xFEBC673Cu,
        0x80802E60u,
        0x8CAC3BD9u,
        0x61CC29E6u,
        0x4FB4DA20u,
        0x0F2CC0D1u,
        0x7543CFBCu,
        0x5E75012Au,
        0xC7AD6297u,
        0x8F009507u,
        0x83660755u,
        0xB74B05F4u
    }};
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto* descriptor = FindRenoDxFallout4Shader(expected[index]);
        FO4HDR_CHECK(descriptor != nullptr);
        FO4HDR_CHECK(descriptor->crc32 == expected[index]);
        FO4HDR_CHECK(
            static_cast<std::size_t>(descriptor->pass) == index);
    }
    for (const auto crc32 : {0x7543CFBCu, 0x5E75012Au, 0xC7AD6297u})
    {
        const auto* descriptor = FindRenoDxFallout4Shader(crc32);
        FO4HDR_CHECK(descriptor != nullptr);
        FO4HDR_CHECK(
            descriptor->requirement == PipelinePassRequirement::optional);
        FO4HDR_CHECK(descriptor->group == PipelinePassGroup::postEffect);
    }
    FO4HDR_CHECK(FindRenoDxFallout4Shader(0xFFFFFFFFu) == nullptr);

    FO4HDR_CHECK(kRenoDxFallout4Shaders[0].sceneInputSlot == 1);
    FO4HDR_CHECK(kRenoDxFallout4Shaders[1].sceneInputSlot == 1);
    FO4HDR_CHECK(kRenoDxFallout4Shaders[2].sceneInputSlot == 0);
    FO4HDR_CHECK(kRenoDxFallout4Shaders[3].sceneInputSlot == 0);
    FO4HDR_CHECK(
        kRenoDxFallout4Shaders[4].group ==
        PipelinePassGroup::creativeGrade);
}

void TestUnknownOutcomes()
{
    FO4HDR_CHECK(
        DecideUnknownShaderOutcome(PipelinePassRequirement::untracked) ==
        UnknownShaderOutcome::passThrough);
    FO4HDR_CHECK(
        DecideUnknownShaderOutcome(PipelinePassRequirement::optional) ==
        UnknownShaderOutcome::disableOptionalFeature);
    FO4HDR_CHECK(
        DecideUnknownShaderOutcome(PipelinePassRequirement::required) ==
        UnknownShaderOutcome::disableHdrGeneration);
}

void TestHeartbeat()
{
    PipelineHeartbeat heartbeat;
    heartbeat.Reset(7);
    heartbeat.BeginFrame(7, 11);
    heartbeat.Observe(RenoDxShaderPass::toneMapNewB);
    auto state = heartbeat.Evaluate(7, 11);
    FO4HDR_CHECK(state.currentGeneration);
    FO4HDR_CHECK(state.currentFrame);
    FO4HDR_CHECK(state.toneMapFamilyObserved);
    FO4HDR_CHECK(!state.lutObserved);
    FO4HDR_CHECK(!state.ready);

    heartbeat.Observe(RenoDxShaderPass::lut);
    state = heartbeat.Evaluate(7, 11);
    FO4HDR_CHECK(state.ready);
    FO4HDR_CHECK(!heartbeat.Evaluate(7, 12).ready);
    FO4HDR_CHECK(!heartbeat.Evaluate(8, 11).ready);

    heartbeat.BeginFrame(7, 12);
    state = heartbeat.Evaluate(7, 12);
    FO4HDR_CHECK(!state.toneMapFamilyObserved);
    FO4HDR_CHECK(!state.lutObserved);
    FO4HDR_CHECK(!state.ready);

    heartbeat.BeginFrame(8, 1);
    heartbeat.Observe(RenoDxShaderPass::lut);
    FO4HDR_CHECK(!heartbeat.Evaluate(8, 1).ready);
}

void TestSceneReplacementAllowed()
{
    using enum RenoDxShaderPass;
    PipelineHeartbeat heartbeat;
    heartbeat.Reset(3);
    heartbeat.BeginFrame(3, 5);
    const auto empty = heartbeat.Evaluate(3, 5);
    heartbeat.Observe(lut);
    const auto lutOnly = heartbeat.Evaluate(3, 5);
    heartbeat.BeginFrame(3, 5);
    heartbeat.Observe(toneMapA);
    const auto toneMapped = heartbeat.Evaluate(3, 5);
    for (const auto pass : {lut, taa, taa2, fxaa})
    {
        FO4HDR_CHECK(!SceneReplacementAllowed(pass, empty));
        FO4HDR_CHECK(!SceneReplacementAllowed(pass, lutOnly));
        // A replaced tone map keeps the chain replaced even when its LUT is missed.
        FO4HDR_CHECK(SceneReplacementAllowed(pass, toneMapped));
        FO4HDR_CHECK(!SceneReplacementAllowed(pass, heartbeat.Evaluate(3, 6)));
        FO4HDR_CHECK(!SceneReplacementAllowed(pass, heartbeat.Evaluate(4, 5)));
    }
    for (const auto pass : {toneMapA, toneMapNewB, video, dofBlender, gameDof})
        FO4HDR_CHECK(SceneReplacementAllowed(pass, empty));
}

// Hosted HDR replaces the tone map and the LUT that follows it, and nothing else.
void TestHostedDrawDecision()
{
    using enum RenoDxShaderPass;
    using Target = HostedTarget;
    using Action = HostedDrawAction;
    PipelineHeartbeat heartbeat;
    heartbeat.Reset(3);
    heartbeat.BeginFrame(3, 5);
    const auto empty = heartbeat.Evaluate(3, 5);
    heartbeat.Observe(toneMapNewA);
    const auto toneMapped = heartbeat.Evaluate(3, 5);
    for (const auto pass : {toneMapA, toneMapB, toneMapNewA, toneMapNewB})
    {
        FO4HDR_CHECK(DecideHostedDraw(pass, Target::workingR11g11b10, empty) == Action::replace);
        FO4HDR_CHECK(DecideHostedDraw(pass, Target::rgba16Float, empty) == Action::replace);
        // A target that cannot carry the unbounded scene keeps the game's tone map.
        FO4HDR_CHECK(DecideHostedDraw(pass, Target::other, empty) == Action::native);
    }
    // The LUT waits for this frame's tone map and encodes for what its target can store.
    for (const auto target : {Target::other, Target::workingR11g11b10, Target::rgba16Float})
    {
        FO4HDR_CHECK(DecideHostedDraw(lut, target, empty) == Action::native);
        FO4HDR_CHECK(DecideHostedDraw(lut, target, heartbeat.Evaluate(3, 6)) == Action::native);
    }
    FO4HDR_CHECK(DecideHostedDraw(lut, Target::rgba16Float, toneMapped) == Action::replace);
    FO4HDR_CHECK(DecideHostedDraw(lut, Target::workingR11g11b10, toneMapped) ==
        Action::replaceContained);
    FO4HDR_CHECK(DecideHostedDraw(lut, Target::other, toneMapped) == Action::replaceContained);
    // Everything after the LUT belongs to the host.
    for (const auto pass : {taa, taa2, fxaa, video, dofBlender, gameDof})
        FO4HDR_CHECK(DecideHostedDraw(pass, Target::rgba16Float, toneMapped) == Action::native);
}

// The hosted scene decodes back to the absolute light it was made from, keeps
// colors outside BT.709 as negative values, and loses no single channel when
// the target cannot store them.
void TestHostedSceneEncoding()
{
    const auto decode = [](double code) { return std::copysign(std::pow(std::abs(code), 2.2), code); };
    for (const double paperWhite : {100.0, 203.0, 300.0})
    {
        const auto white = EncodeHostedScene({paperWhite, paperWhite, paperWhite}, paperWhite, false);
        // The binary32 matrix rows sum to one only within single precision.
        FO4HDR_CHECK(test::Approx(white.red, 1.0, 1.0e-6) && test::Approx(white.blue, 1.0, 1.0e-6));
        // The host multiplies by its paper white again, so absolute nits return.
        const LinearRgb highlight{900.0, 650.0, 120.0};
        const auto encoded = EncodeHostedScene(highlight, paperWhite, false);
        const auto restored = ConvertBt709ToBt2020({decode(encoded.red) * paperWhite,
            decode(encoded.green) * paperWhite, decode(encoded.blue) * paperWhite});
        FO4HDR_CHECK(test::Approx(restored.red, highlight.red, 1.0e-9));
        FO4HDR_CHECK(test::Approx(restored.green, highlight.green, 1.0e-9));
        FO4HDR_CHECK(test::Approx(restored.blue, highlight.blue, 1.0e-9));
    }
    // A pure BT.2020 green lies outside BT.709: red and blue go negative.
    const LinearRgb green{0.0, 203.0, 0.0};
    const auto signedGreen = EncodeHostedScene(green, 203.0, false);
    FO4HDR_CHECK(signedGreen.red < 0.0 && signedGreen.blue < 0.0 && signedGreen.green > 1.0);
    const auto contained = EncodeHostedScene(green, 203.0, true);
    FO4HDR_CHECK(std::min({contained.red, contained.green, contained.blue}) >= 0.0);
    FO4HDR_CHECK(test::Approx(std::min(contained.red, contained.blue), 0.0, 1.0e-12));
    // Containment keeps BT.709 luminance and leaves an in-gamut color alone.
    const auto luminance = [&](LinearRgb code) {
        return 0.2126 * decode(code.red) + 0.7152 * decode(code.green) + 0.0722 * decode(code.blue);
    };
    FO4HDR_CHECK(test::Approx(luminance(contained), luminance(signedGreen), 1.0e-9));
    const LinearRgb neutral{150.0, 150.0, 150.0};
    const auto plain = EncodeHostedScene(neutral, 203.0, false);
    const auto same = EncodeHostedScene(neutral, 203.0, true);
    FO4HDR_CHECK(test::Approx(plain.red, same.red, 1.0e-12) && test::Approx(plain.blue, same.blue, 1.0e-12));
    // Invalid white and non-finite light are contained.
    FO4HDR_CHECK(EncodeHostedScene(neutral, 0.0, false).green == 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(EncodeHostedScene(neutral, nan, false).green == 0.0);
    const auto bounded = EncodeHostedScene({nan, std::numeric_limits<double>::infinity(), 0.0}, 203.0, true);
    FO4HDR_CHECK(std::isfinite(bounded.red) && std::isfinite(bounded.green) && std::isfinite(bounded.blue));
}

// Hosted HDR latches only on a complete answer and the host's HDR container.
void TestHostedHdrDecision()
{
    const HostHdrOutputState hdr{sizeof(HostHdrOutputState), 1U, 203.0F, 1000.0F};
    FO4HDR_CHECK(HostReportsHdr(&hdr));
    FO4HDR_CHECK(!HostReportsHdr(nullptr));
    auto state = hdr;
    state.hdrActive = 0U;
    FO4HDR_CHECK(!HostReportsHdr(&state));
    state = hdr;
    state.size = sizeof(HostHdrOutputState) - 1U;
    FO4HDR_CHECK(!HostReportsHdr(&state));
    for (const float white : {0.0F, -1.0F, 20000.0F, std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity()})
    {
        state = hdr;
        state.paperWhiteNits = white;
        FO4HDR_CHECK(!HostReportsHdr(&state));
    }
    const HostedHdrInputs latched{.hostedHdrRequested = true, .hostReportsHdr = true,
        .chainRgba8Unorm = true, .backBufferFp16 = true};
    FO4HDR_CHECK(DecideHostedHdr(latched));
    const auto without = [&](bool HostedHdrInputs::* flag, bool value) {
        auto inputs = latched;
        inputs.*flag = value;
        return DecideHostedHdr(inputs);
    };
    FO4HDR_CHECK(!without(&HostedHdrInputs::hostedHdrRequested, false));
    FO4HDR_CHECK(!without(&HostedHdrInputs::hostReportsHdr, false));
    // A plain chain or the host's SDR container cannot carry the hosted scene.
    FO4HDR_CHECK(!without(&HostedHdrInputs::chainRgba8Unorm, false));
    FO4HDR_CHECK(!without(&HostedHdrInputs::backBufferFp16, false));
    FO4HDR_CHECK(!without(&HostedHdrInputs::enbDetected, true));
    FO4HDR_CHECK(!without(&HostedHdrInputs::configurationDefaults, true));
}

void TestNativeImageSpacePassResolution()
{
    constexpr auto bits = [](auto... passes) { return (RenoDxShaderBit(passes) | ...); };
    using enum RenoDxShaderPass;
    using Scope = NativeImageSpacePass;
    constexpr auto updated = bits(toneMapNewA, toneMapNewB, lut);
    constexpr auto original = bits(toneMapA, toneMapB, lut);
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::toneMapCinematic, updated) == toneMapNewA);
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::toneMapCinematicFade, updated) == toneMapNewB);
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::lut, updated) == lut);
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::toneMapCinematic, original) == toneMapA);
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::toneMapCinematicFade, original) == toneMapB);
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::lut, original | bits(taa)) == lut);

    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::none, updated));
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::toneMapCinematic, bits(toneMapNewA, toneMapNewB)));
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::toneMapCinematic, bits(toneMapNewA, lut)));
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::toneMapCinematic, bits(toneMapNewA, toneMapB, lut)));
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::lut, updated | original));
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::lut, updated | bits(toneMapA)));
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(Scope::lut, bits(lut, taa, fxaa)));

    // An effect scope takes its one pass only while that stock pass is registered.
    constexpr auto everyPass = bits(toneMapA, toneMapB, toneMapNewA, toneMapNewB, lut, taa, taa2, fxaa, video,
        dofAuxiliary1, dofAuxiliary2, dofAuxiliary3, dofBlender, gameDof, vignette);
    for (const auto& [scope, pass] : std::array{std::pair{Scope::temporalAa, taa2},
             std::pair{Scope::temporalAaPipboy, taa2}, std::pair{Scope::temporalAaPowerArmorPipboy, taa},
             std::pair{Scope::bokehDofBlend, dofBlender}, std::pair{Scope::distantBlur, gameDof},
             std::pair{Scope::fxaa, fxaa}})
    {
        FO4HDR_CHECK(ResolveNativeImageSpacePass(scope, RenoDxShaderBit(pass)) == pass);
        FO4HDR_CHECK(!ResolveNativeImageSpacePass(scope, everyPass & ~RenoDxShaderBit(pass)));
    }
    FO4HDR_CHECK(ResolveNativeImageSpacePass(Scope::lut, everyPass & ~bits(toneMapA, toneMapB)) == lut);
    FO4HDR_CHECK(!ResolveNativeImageSpacePass(static_cast<Scope>(10), everyPass));
}

void TestDeveloperConfiguration()
{
    constexpr std::string_view disabled = R"ini(
[General]
HdrEnabled=1
DeveloperMode=0

[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ForceHdrSwapChainFailure=0
)ini";
    auto parsed = ParsePluginConfiguration(disabled);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.developerMode);
    FO4HDR_CHECK(!parsed.value.traceRenderPipeline);

    constexpr std::string_view enabled = R"ini(
[General]
HdrEnabled=1
DeveloperMode=1
[Debug]
TraceRenderPipeline=1
DeveloperHdr10Reference=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(enabled);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(parsed.value.developerMode);
    FO4HDR_CHECK(parsed.value.traceRenderPipeline);

    constexpr std::string_view unsafe = R"ini(
[General]
HdrEnabled=1
DeveloperMode=0
[Debug]
TraceRenderPipeline=1
DeveloperHdr10Reference=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(unsafe);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(
        parsed.error ==
        PluginConfigurationError::invalidCombination);

    constexpr std::string_view duplicate = R"ini(
[General]
HdrEnabled=1
DeveloperMode=1
DeveloperMode=1
[Debug]
TraceRenderPipeline=1
DeveloperHdr10Reference=0
ForceHdrSwapChainFailure=0
)ini";
    parsed = ParsePluginConfiguration(duplicate);
    FO4HDR_CHECK(!parsed.valid);
    FO4HDR_CHECK(
        parsed.error ==
        PluginConfigurationError::duplicateKey);
}

}  // namespace

void RunPipelineContractTests()
{
    TestRenoDxCrcDomain();
    TestTrustedCatalog();
    TestUnknownOutcomes();
    TestHeartbeat();
    TestSceneReplacementAllowed();
    TestHostedDrawDecision();
    TestHostedSceneEncoding();
    TestHostedHdrDecision();
    TestNativeImageSpacePassResolution();
    TestDeveloperConfiguration();
}

}  // namespace fo4hdr::tests
