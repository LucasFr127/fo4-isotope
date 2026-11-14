// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
    // Without DeveloperMode the trace switch keeps its saved value and has no effect.
    parsed = ParsePluginConfiguration(unsafe);
    FO4HDR_CHECK(parsed.valid);
    FO4HDR_CHECK(!parsed.value.traceRenderPipeline && parsed.stored.traceRenderPipeline);

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
    // A repeated key keeps its last value.
    parsed = ParsePluginConfiguration(duplicate);
    FO4HDR_CHECK(parsed.valid && parsed.value.developerMode && parsed.value.traceRenderPipeline);
}

}  // namespace

void RunPipelineContractTests()
{
    TestRenoDxCrcDomain();
    TestTrustedCatalog();
    TestUnknownOutcomes();
    TestHeartbeat();
    TestSceneReplacementAllowed();
    TestNativeImageSpacePassResolution();
    TestDeveloperConfiguration();
}

}  // namespace fo4hdr::tests
