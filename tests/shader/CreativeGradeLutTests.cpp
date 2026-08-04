// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
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
    const OperatorLutData& table, float strength)
{
    auto& fixture = inputs.fixture;
    FO4HDR_CHECK(presentation.ApplySettings(table, fixture.whites,
        PresentationScope::qualifiedExclusive, {strength}));
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    Pixels source;
    source.fill(sourceColor);
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, source);
    inputs.Bind();
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    inputs.Check();
    const auto expected = fo4hdr::test::SampleOperatorLut(table, ExpectedGrade(inputs, strength));
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
        VerifyGradeStrength(inputs, presentation, table, strength);
    FO4HDR_CHECK(presentation.Retire());
}

}  // namespace fo4hdr::tests::gpu
