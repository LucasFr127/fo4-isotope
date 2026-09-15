// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include "passes/PresentationPass.hpp"
#include "reference/OperatorLutTestSupport.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cmath>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;
constexpr Float4 sourceColor{2, 4, 1, 0.75f};
constexpr float displayPeak = 1000;
constexpr float uiWhite = 203;

struct WeightedGradeInputs
{
    PixelFixture& fixture;
    ComPtr<ID3D11DeviceContext1> context;
    std::array<LutPixels, 4> pixels;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> luts;
    ComPtr<ID3D11Buffer> nativeConstants;
    ComPtr<ID3D11Buffer> borrowedConstants;

    explicit WeightedGradeInputs(PixelFixture& owner) : fixture(owner)
    {
        CheckHresult(owner.gpu.context.As(&context), "Grade Context1");
        for (unsigned index = 0; index < luts.size(); ++index)
        {
            pixels[index] = LutInputs(index);
            luts[index] = LutTexture(owner.gpu.device.Get(), pixels[index]);
        }
        const std::array<Float4, 2> weights{{{}, {0.25f, 0.5f, 0.75f, 0.125f}}};
        nativeConstants = ConstantBuffer(owner.gpu.device.Get(), weights);
        std::array<Float4, 32> borrowed{};
        borrowed[16] = {9, 8, 7, 6};
        borrowedConstants = ConstantBuffer(owner.gpu.device.Get(), borrowed);
    }

    void Bind()
    {
        fixture.Bind(true);
        const std::array<ID3D11ShaderResourceView*, 8> views{
            fixture.renderer.Views(fixture.input).shaderResource, nullptr, nullptr,
            luts[0].Get(), luts[1].Get(), luts[2].Get(), luts[3].Get(), luts[0].Get()};
        context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        const std::array<ID3D11SamplerState*, 8> samplers{
            fixture.point.Get(), nullptr, nullptr, fixture.linear.Get(), fixture.linear.Get(),
            fixture.linear.Get(), fixture.linear.Get(), fixture.point.Get()};
        context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
        auto* native = nativeConstants.Get();
        context->PSSetConstantBuffers(2, 1, &native);
        auto* borrowed = borrowedConstants.Get();
        const UINT first = 16;
        const UINT count = 16;
        context->PSSetConstantBuffers1(13, 1, &borrowed, &first, &count);
    }

    void Check()
    {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> source;
        ComPtr<ID3D11SamplerState> sampler;
        UINT first{};
        UINT count{};
        context->PSGetConstantBuffers(13, 1, &buffer);
        context->PSGetConstantBuffers1(13, 1, nullptr, &first, &count);
        context->PSGetShaderResources(7, 1, &source);
        context->PSGetSamplers(7, 1, &sampler);
        FO4HDR_CHECK(buffer.Get() == borrowedConstants.Get());
        FO4HDR_CHECK(first == 16);
        FO4HDR_CHECK(count == 16);
        FO4HDR_CHECK(source.Get() == luts[0].Get());
        FO4HDR_CHECK(sampler.Get() == fixture.point.Get());
        CheckBoundInputs(fixture, 0, nativeConstants.Get());
    }
};

LinearRgb ExpectedGrade(const WeightedGradeInputs& inputs, float strength)
{
    const auto endpoint = LutExpected(sourceColor, inputs.pixels);
    return {std::lerp(static_cast<double>(sourceColor.red), static_cast<double>(endpoint.red), strength),
        std::lerp(static_cast<double>(sourceColor.green), static_cast<double>(endpoint.green), strength),
        std::lerp(static_cast<double>(sourceColor.blue), static_cast<double>(endpoint.blue), strength)};
}

LinearRgb ExpectedGradeRemoval(LinearRgb graded, const HdrSettingsMath& controls)
{
    if (controls.vanillaGradeLuminanceRemoval == 0 && controls.vanillaGradeColorRemoval == 0)
        return graded;
    // This positive fixture oracle independently separates scalar luma and normalized color.
    constexpr std::array weights{0.2125, 0.7154, 0.0721};
    const double originalLuma = sourceColor.red * weights[0] +
        sourceColor.green * weights[1] + sourceColor.blue * weights[2];
    const double gradedLuma = graded.red * weights[0] + graded.green * weights[1] + graded.blue * weights[2];
    const double targetLuma = std::lerp(gradedLuma, originalLuma, controls.vanillaGradeLuminanceRemoval);
    const std::array original{sourceColor.red, sourceColor.green, sourceColor.blue};
    const std::array channels{graded.red, graded.green, graded.blue};
    std::array<double, 3> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = targetLuma * std::lerp(channels[index] / gradedLuma,
            original[index] / originalLuma, controls.vanillaGradeColorRemoval);
    return {result[0], result[1], result[2]};
}

void CheckGradeChannel(float actual, double expectedNits)
{
    const auto expected = expectedNits / uiWhite;
    // Same packed coefficients; sampling allowance plus the existing coarse FP16 allowance.
    const auto tolerance = 0.5 / uiWhite + std::abs(expected) * 2 / 1024 + 0.002;
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(actual >= 0);
    FO4HDR_CHECK(actual <= displayPeak / uiWhite);
    FO4HDR_CHECK(std::abs(static_cast<double>(actual) - expected) <= tolerance);
}

void VerifyGradeStrength(WeightedGradeInputs& inputs, PresentationPass& presentation,
    const OperatorLutData& table, const SceneGradeConfiguration& configuration)
{
    const auto strength = configuration.vanillaColorGradingStrength;
    const auto& selective = configuration.selectiveColor;
    auto& fixture = inputs.fixture;
    FO4HDR_CHECK(presentation.ApplySettings(table, fixture.whites,
        PresentationScope::qualifiedExclusive, configuration));
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    Pixels source;
    source.fill(sourceColor);
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, source);
    inputs.Bind();
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    inputs.Check();
    const auto grade = ExpectedGradeRemoval(ExpectedGrade(inputs, strength), configuration.imageControls);
    SelectiveColorPrepared prepared;
    FO4HDR_CHECK(PrepareSelectiveColor(selective, prepared));
    const auto adjusted = ApplySelectiveColor({grade.red, grade.green, grade.blue}, prepared);
    const auto expected = fo4hdr::test::SampleOperatorLut(table,
        {adjusted.red, adjusted.green, adjusted.blue});
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : output)
    {
        CheckGradeChannel(pixel.red, expected.red);
        CheckGradeChannel(pixel.green, expected.green);
        CheckGradeChannel(pixel.blue, expected.blue);
        FO4HDR_CHECK(pixel.alpha == sourceColor.alpha);
    }
    if (strength == 0) FO4HDR_CHECK(std::abs(output.front().green - sourceColor.green) > 0.1f);
}

enum class ProfileDrawCase { matched, unmatched, revoked };

void VerifyLutProfiles(WeightedGradeInputs& inputs, PresentationPass& presentation,
    const OperatorLutData& table)
{
    auto& fixture = inputs.fixture;
    SceneGradeConfiguration configuration{0.5F};
    configuration.imageControls.vanillaGradeLuminanceRemoval = 0.35;
    configuration.imageControls.vanillaGradeColorRemoval = 0.8;
    auto& profiles = configuration.lutGradeProfiles;
    profiles.count = 1;
    auto& profile = profiles.entries[0];
    constexpr std::string_view path{"textures/luts/authored-profile.dds"};
    FO4HDR_CHECK(NormalizeLutGradePath(path, profile.path));
    profile.luminancePercent = profile.colorPercent = 0;
    profile.overrideLuminance = profile.overrideColor = true;
    FO4HDR_CHECK(presentation.ApplySettings(table, fixture.whites,
        PresentationScope::qualifiedExclusive, configuration));
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    Pixels source;
    source.fill(sourceColor);
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, source);
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    for (const auto test : {ProfileDrawCase::matched, ProfileDrawCase::unmatched, ProfileDrawCase::revoked})
    {
        inputs.Bind();
        NativeLutObservation observation;
        observation.resourceGeneration = NativeLutResourceGeneration();
        observation.generation = NativeLutObservationGeneration();
        observation.threadId = GetCurrentThreadId();
        observation.validSlots = 1;
        observation.luts[0].utf8Path = profile.path;
        observation.luts[0].pathLength = static_cast<std::uint16_t>(path.size());
        observation.luts[0].weight = 0.25F;
        observation.expectedShaderViews[0] = reinterpret_cast<std::uintptr_t>(
            inputs.luts[test == ProfileDrawCase::unmatched ? 1 : 0].Get());
        NativeLutScope scope(observation);
        if (test == ProfileDrawCase::revoked) RevokeNativeLutObservation();
        FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
        inputs.Check();
        const auto copied = fixture.renderer.CopyLutInspector();
        FO4HDR_CHECK(copied.validatedSlots == (test == ProfileDrawCase::matched ? 1u : 0u));
        auto controls = configuration.imageControls;
        if (test == ProfileDrawCase::matched)
        {
            // Actual native weights sum to 1.625; only slot zero overrides globals.
            controls.vanillaGradeLuminanceRemoval = (0.25 + 1.375 * 0.35) / 1.625;
            controls.vanillaGradeColorRemoval = (0.25 + 1.375 * 0.8) / 1.625;
        }
        const auto graded = ExpectedGradeRemoval(ExpectedGrade(inputs, 0.5F), controls);
        const auto expected = fo4hdr::test::SampleOperatorLut(table, graded);
        const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
        for (const auto pixel : output)
        {
            CheckGradeChannel(pixel.red, expected.red);
            CheckGradeChannel(pixel.green, expected.green);
            CheckGradeChannel(pixel.blue, expected.blue);
            FO4HDR_CHECK(pixel.alpha == sourceColor.alpha);
        }
    }
    FO4HDR_CHECK(fixture.renderer.EndFrame());
}

}  // namespace

void VerifyCreativeGradeLut(const std::filesystem::path& fixtureDirectory)
{
    const auto table = BuildOperatorLut(OperatorLutAlgorithm::gt7, displayPeak);
    FO4HDR_CHECK(table.IsValid());
    if (!table.IsValid()) return;
    PixelFixture fixture(fixtureDirectory, width, height, {uiWhite, 203}, &table);
    PresentationPass presentation;
    const PresentationConfiguration configuration{width, height, displayPeak, &fixture.renderer, fixture.whites};
    FO4HDR_CHECK(presentation.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    WeightedGradeInputs inputs(fixture);
    for (const float strength : {0.0f, 0.5f, 1.0f})
        VerifyGradeStrength(inputs, presentation, table, {strength});
    SelectiveColorSettings selective;
    selective.regions.fill({3, 125, 30});
    VerifyGradeStrength(inputs, presentation, table, {0, {}, ColorGradingMethod::radial, selective});
    selective.enabled = true;
    VerifyGradeStrength(inputs, presentation, table, {0, {}, ColorGradingMethod::radial, selective});
    SceneGradeConfiguration removal{0.5F};
    removal.imageControls.vanillaGradeLuminanceRemoval = 0.35;
    removal.imageControls.vanillaGradeColorRemoval = 0.8;
    VerifyGradeStrength(inputs, presentation, table, removal);
    VerifyLutProfiles(inputs, presentation, table);
    FO4HDR_CHECK(presentation.Retire());
}

}  // namespace fo4hdr::tests::gpu
