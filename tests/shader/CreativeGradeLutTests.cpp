// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "FO4Isotope/output/SelectiveColor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace fo4hdr::tests::gpu {

std::array<fo4hdr::LinearRgb, kWidth> ReadCreativeGradedScene(
    const Float4* gradedValues, const std::array<Float4, kWidth>& corpus)
{
    std::array<fo4hdr::LinearRgb, kWidth> gradedScene{};
    for (std::size_t index = 0; index < kWidth; ++index) {
        gradedScene[index] = {
            gradedValues[index].red, gradedValues[index].green,
            gradedValues[index].blue};
        const std::array actual{
            gradedScene[index].red, gradedScene[index].green,
            gradedScene[index].blue};
        for (std::size_t channel = 0; channel < actual.size(); ++channel) {
            FO4HDR_WARP_CHECK(std::isfinite(actual[channel]));
            FO4HDR_WARP_CHECK(actual[channel] >= 0.0);
        }
        FO4HDR_WARP_CHECK(gradedValues[index].alpha == corpus[index].alpha);
    }
    return gradedScene;
}

void VerifyCreativeUngradedPixels(
    const Float4* gradedValues,
    const std::array<std::array<std::uint16_t, 4>, kWidth>& scenePixels)
{
    for (std::size_t index = 0; index < kWidth; ++index) {
        const std::array expected{
            Binary16ToFloat(scenePixels[index][0]),
            Binary16ToFloat(scenePixels[index][1]),
            Binary16ToFloat(scenePixels[index][2])};
        const std::array actual{
            gradedValues[index].red,
            gradedValues[index].green,
            gradedValues[index].blue};
        for (std::size_t channel = 0; channel < actual.size(); ++channel)
            FO4HDR_WARP_CHECK(std::abs(
                actual[channel] - expected[channel]) <= 1.0e-6F);
    }
}

void VerifyCreativeHalfGradePixels(
    const Float4* gradedValues,
    const std::array<std::array<std::uint16_t, 4>, kWidth>& scenePixels,
    const std::array<fo4hdr::LinearRgb, kWidth>& gradedScene)
{
    for (std::size_t index = 0; index < kWidth; ++index) {
        const std::array ungraded{
            static_cast<double>(Binary16ToFloat(scenePixels[index][0])),
            static_cast<double>(Binary16ToFloat(scenePixels[index][1])),
            static_cast<double>(Binary16ToFloat(scenePixels[index][2]))};
        const std::array graded{
            gradedScene[index].red,
            gradedScene[index].green,
            gradedScene[index].blue};
        const std::array actual{
            gradedValues[index].red,
            gradedValues[index].green,
            gradedValues[index].blue};
        for (std::size_t channel = 0; channel < actual.size(); ++channel) {
            const auto expected =
                std::lerp(ungraded[channel], graded[channel], 0.5);
            FO4HDR_WARP_CHECK(std::abs(
                static_cast<double>(actual[channel]) - expected) <= 2.0e-5);
        }
    }
}

void VerifyCreativeDesaturatedPixels(const Float4* gradedValues)
{
    for (std::size_t index = 0; index < kWidth; ++index) {
        FO4HDR_WARP_CHECK(std::isfinite(gradedValues[index].red));
        FO4HDR_WARP_CHECK(std::abs(
            gradedValues[index].red - gradedValues[index].green) <= 1.0e-5F);
        FO4HDR_WARP_CHECK(std::abs(
            gradedValues[index].green - gradedValues[index].blue) <= 1.0e-5F);
        if (index == 3U)
            FO4HDR_WARP_CHECK(gradedValues[index].red > 1.0F);
    }
}

void VerifyCreativeOversaturatedPixels(
    const Float4* gradedValues,
    const std::array<std::array<std::uint16_t, 4>, kWidth>& scenePixels)
{
    constexpr std::array<float, 3> bt709Luma{
        0.212500006F, 0.715399981F, 0.0720999986F};
    for (std::size_t index = 0; index < kWidth; ++index) {
        const std::array input{
            Binary16ToFloat(scenePixels[index][0]),
            Binary16ToFloat(scenePixels[index][1]),
            Binary16ToFloat(scenePixels[index][2])};
        const std::array adjusted{
            gradedValues[index].red,
            gradedValues[index].green,
            gradedValues[index].blue};
        float inputLuminance{};
        float outputLuminance{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            FO4HDR_WARP_CHECK(std::isfinite(adjusted[channel]));
            FO4HDR_WARP_CHECK(adjusted[channel] >= 0.0F);
            inputLuminance += input[channel] * bt709Luma[channel];
            outputLuminance += adjusted[channel] * bt709Luma[channel];
        }
        FO4HDR_WARP_CHECK(std::abs(
            outputLuminance - inputLuminance) <=
            std::max(1.0e-4F, inputLuminance * 2.0e-3F));
    }
}

std::array<fo4hdr::SelectiveColorSettings, 5> CreativeSelectiveCases()
{
    fo4hdr::SelectiveColorSettings selective{};
    selective.enabled = true;
    selective.regions[0] = {8.0, 125.0, 20.0};
    selective.regions[1] = {-5.0, 90.0, -10.0};
    selective.regions[5] = {3.0, 115.0, 30.0};
    std::array<fo4hdr::SelectiveColorSettings, 5> selectiveCases{};
    selectiveCases[0] = selective;
    selectiveCases[1].enabled = true;
    for (std::size_t region = 0; region < selective.regions.size(); ++region)
        selectiveCases[1].regions[region] = {
            region % 2 == 0 ? -10.0 : 10.0,
            region % 3 == 0 ? 50.0 : 150.0,
            region % 2 == 0 ? 100.0 : -100.0};
    selectiveCases[2] = selectiveCases[1];
    selectiveCases[2].amountPercent = 37.0;
    selectiveCases[3] = selectiveCases[1];
    selectiveCases[3].enabled = false;
    selectiveCases[4] = selectiveCases[1];
    selectiveCases[4].amountPercent = 0.0;
    return selectiveCases;
}

std::array<std::array<Float4, kWidth>, 4> CreativeSelectiveInputs(
    const std::array<Float4, kWidth>& corpus)
{
    std::array<std::array<Float4, kWidth>, 4> selectiveInputs{};
    selectiveInputs[0] = corpus;
    // Midpoints of the seven region intervals, plus the hue-wrap boundary.
    constexpr std::array<double, kWidth> boundaryDegrees{
        15.0, 45.0, 90.0, 150.0, 210.0, 270.0, 330.0, 360.0};
    for (std::size_t index = 0; index < kWidth; ++index)
    {
        const double angle = boundaryDegrees[index] * std::acos(-1.0) / 180.0;
        const auto colorAt = [&](double mean, double radius) {
            return Float4{
                static_cast<float>(mean + 2.0 * radius * std::cos(angle)),
                static_cast<float>(mean - radius * std::cos(angle) +
                    std::sqrt(3.0) * radius * std::sin(angle)),
                static_cast<float>(mean - radius * std::cos(angle) -
                    std::sqrt(3.0) * radius * std::sin(angle)), 0.5F};
        };
        selectiveInputs[1][index] = colorAt(0.4, 0.12);
        selectiveInputs[2][index] = colorAt(1.0, 0.001);
    }
    // Negative channels exercise the creative shader's max-to-zero input seam.
    // NaN/Inf are outside the selective helper's sanitized finite-input contract.
    selectiveInputs[3] = {{
        {65504.0F, 65504.0F, 65504.0F, 0.25F},
        {65504.0F, 32752.0F, 16376.0F, 0.5F},
        {16376.0F, 65504.0F, 32752.0F, 0.75F},
        {32752.0F, 16376.0F, 65504.0F, 1.0F},
        {0.00006103515625F, 0.000030517578125F, 0.0F, 0.5F},
        {-1.0F, 0.5F, 2.0F, 0.5F},
        {0.5F, -1.0F, 2.0F, 0.5F},
        {0.5F, 2.0F, -1.0F, 0.5F},
    }};
    return selectiveInputs;
}

struct CreativeSelectiveReport
{
    std::size_t settingIndex{};
    std::size_t batch{};
    double maximumNormalizedError{};
    unsigned maximumHalfDistance{};
};
void VerifyCreativeSelectiveBatch(
    const std::array<std::array<std::uint16_t, 4>, kWidth>& pixels,
    const Float4* gradedValues,
    const fo4hdr::SelectiveColorPrepared& selectivePrepared,
    CreativeSelectiveReport& report)
{
    for (std::size_t index = 0; index < kWidth; ++index)
    {
        const fo4hdr::SelectiveColorRgb input{
            std::max(0.0F, Binary16ToFloat(pixels[index][0])),
            std::max(0.0F, Binary16ToFloat(pixels[index][1])),
            std::max(0.0F, Binary16ToFloat(pixels[index][2]))};
        const auto expected = fo4hdr::ApplySelectiveColor(
            input, selectivePrepared);
        const std::array actual{gradedValues[index].red,
            gradedValues[index].green, gradedValues[index].blue};
        const std::array expectedValues{expected.red, expected.green,
            expected.blue};
        const auto inputScale =
            std::max({1.0, input.red, input.green, input.blue});
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const auto error = std::abs(static_cast<double>(actual[channel]) -
                expectedValues[channel]) / inputScale;
            const auto actualHalf = FloatToBinary16(actual[channel]);
            const auto expectedHalf = FloatToBinary16(
                static_cast<float>(expectedValues[channel]));
            const auto distance = CodeDistance(actualHalf, expectedHalf);
            report.maximumNormalizedError =
                std::max(report.maximumNormalizedError, error);
            report.maximumHalfDistance =
                std::max(report.maximumHalfDistance, distance);
            if (error > 2.0e-5 || distance > 2U)
                std::cerr << "Selective parity settings=" << report.settingIndex
                    << " batch=" << report.batch << " sample=" << index
                    << " channel=" << channel << " actual=" << actual[channel]
                    << " expected=" << expectedValues[channel]
                    << " normalized_error=" << error
                    << " half_ulp=" << distance << '\n';
            FO4HDR_WARP_CHECK(std::isfinite(actual[channel]));
            FO4HDR_WARP_CHECK(actual[channel] >= 0.0F);
            FO4HDR_WARP_CHECK(Binary16ToFloat(actualHalf) <= 65504.0F);
            FO4HDR_WARP_CHECK(error <= 2.0e-5);
            FO4HDR_WARP_CHECK(distance <= 2U);
            const bool exactIdentity = !selectivePrepared.effectiveActive ||
                (input.red == input.green && input.green == input.blue);
            if (exactIdentity)
                FO4HDR_WARP_CHECK(actual[channel] == expectedValues[channel]);
        }
        FO4HDR_WARP_CHECK(gradedValues[index].alpha ==
            Binary16ToFloat(pixels[index][3]));
    }
}

struct CreativeOperatorReference
{
    TestedToneMapper algorithm;
    const char* algorithmName;
    float peak;
    float uiWhite;
    const std::array<fo4hdr::LinearRgb, kWidth>& gradedScene;
    const std::array<std::array<std::uint16_t, 4>, kWidth>& scenePixels;
};

void VerifyCreativeOperatorPixels(
    const std::array<std::uint16_t, 4>* values,
    const CreativeOperatorReference& reference)
{
    for (std::size_t index = 0; index < kWidth; ++index) {
        auto expectedNits = MapProductionOperator(
            reference.algorithm, reference.gradedScene[index], reference.peak);
        const std::array expected{
            expectedNits.red / reference.uiWhite,
            expectedNits.green / reference.uiWhite,
            expectedNits.blue / reference.uiWhite};
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            const auto expectedHalf = FloatToBinary16(
                static_cast<float>(expected[channel]));
            const auto actual = Binary16ToFloat(values[index][channel]);
            const auto distance = CodeDistance(
                values[index][channel], expectedHalf);
            const auto lutBound =
                std::max(1.0, static_cast<double>(reference.peak) * 0.005) /
                    reference.uiWhite +
                0.5 * Binary16LargestAdjacentStep(
                    static_cast<float>(expected[channel]));
            const auto acceptable = std::abs(
                static_cast<double>(actual) - expected[channel]) <=
                lutBound;
            if (!acceptable) {
                std::cerr << "CreativeGradeLut "
                          << reference.algorithmName
                          << " parity: sample=" << index
                          << " channel=" << channel
                          << " actual_relative=" << actual
                          << " expected_relative=" << expected[channel]
                          << " half_ulp=" << distance
                          << " lut_bound=" << lutBound << '\n';
            }
            FO4HDR_WARP_CHECK(std::isfinite(actual));
            FO4HDR_WARP_CHECK(actual >= 0.0F);
            FO4HDR_WARP_CHECK(acceptable);
        }
        FO4HDR_WARP_CHECK(
            values[index][3] == reference.scenePixels[index][3]);
    }
}

class CreativeGradeFixture
{
public:
    CreativeGradeFixture(Device& gpu, ID3D11VertexShader* vertex,
        ID3D11SamplerState* sampler) : device(gpu), vertexShader(vertex),
        sceneSampler(sampler) {}

    void Verify()
    {
        Initialize();
        VerifyGradeOnly();
        VerifyZeroGrade();
        VerifyHalfGrade();
        VerifyDesaturation();
        VerifyOversaturation();
        VerifyAdaptation();
        VerifySelectiveColor();
        VerifyOperators();
    }

private:
    static constexpr std::array<Float4, kWidth> corpus{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F},
        {8.0F, 0.0F, 0.0F, 1.0F},
        {0.0F, 8.0F, 0.0F, 1.0F},
        {0.0F, 0.0F, 8.0F, 1.0F},
        {4.0F, 0.5F, 0.125F, 1.0F},
    }};
    static constexpr float peak = 1000.0F;
    static constexpr float uiWhite = 300.0F;
    static constexpr std::array toneMappers{
        TestedToneMapper::aces2Lmt, TestedToneMapper::agX,
        TestedToneMapper::gt7};
    static constexpr std::array algorithmNames{
        "ACES2LMT", "AgX", "GT7"};
    Device& device;
    ID3D11VertexShader* vertexShader;
    ID3D11SamplerState* sceneSampler;
    std::array<std::array<std::uint16_t, 4>, kWidth> scenePixels{};
    D3D11_SUBRESOURCE_DATA sceneData{};
    Texture scene;
    Texture output;
    Texture gradedOutput;
    Texture3D identityLut;
    ComPtr<ID3D11SamplerState> lutSampler;
    ComPtr<ID3D11PixelShader> shader;
    LutConstants lutValues{};
    ComPtr<ID3D11Buffer> lutConstants;
    std::array<ID3D11ShaderResourceView*, 9> resources{};
    std::array<ID3D11SamplerState*, 8> samplers{};
    ID3D11RenderTargetView* target[1]{};
    ComPtr<ID3D11Buffer> gradeOnlyConstants;
    ID3D11RenderTargetView* gradeTarget[1]{};
    D3D11_MAPPED_SUBRESOURCE gradedMapped{};
    const Float4* gradedValues{};
    std::array<fo4hdr::LinearRgb, kWidth> gradedScene{};
    ToneMapConstants ungradedSettings{};
    // Retain scenario buffers through the whole fixture, in creation order,
    // so splitting the draws does not shorten their original COM lifetimes.
    ComPtr<ID3D11Buffer> ungradedConstants;
    ComPtr<ID3D11Buffer> halfGradeConstants;
    ComPtr<ID3D11Buffer> desaturatedConstants;
    ComPtr<ID3D11Buffer> oversaturatedConstants;
    ComPtr<ID3D11Buffer> adaptedConstants;

    void Initialize()
    {
        scenePixels = MakeFp16Pixels(corpus);
        sceneData = {
            scenePixels.data(), static_cast<UINT>(sizeof(scenePixels)), 0};
        scene = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &sceneData);
        output = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET);
        gradedOutput = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D11_BIND_RENDER_TARGET);
        identityLut = CreateIdentityLut(*device.device.Get());
        lutSampler = CreateClampLinearSampler(*device.device.Get());
        shader = CreatePixelShader(
            *device.device.Get(), fo4hdr::generated::kCreativeGradeLut);
        lutValues.values[1] = {1.0F, 0.0F, 0.0F, 0.0F};
        lutConstants = CreateConstantBuffer(
            *device.device.Get(), lutValues);
        resources = {
            scene.srv.Get(), nullptr, nullptr, identityLut.srv.Get(),
            identityLut.srv.Get(), identityLut.srv.Get(),
            identityLut.srv.Get(), nullptr, nullptr};
        samplers = {
            sceneSampler, nullptr, nullptr, lutSampler.Get(), lutSampler.Get(),
            lutSampler.Get(), lutSampler.Get(), nullptr};
        target[0] = output.rtv.Get();
    }

    void ReadGradeOutput(ID3D11Buffer* constants, const char* label)
    {
        DrawFullscreen(*device.context.Get(), { vertexShader, shader.Get(),
            lutConstants.Get(), constants, resources.data(),
            static_cast<UINT>(resources.size()), samplers.data(),
            static_cast<UINT>(samplers.size()), gradeTarget, 1});
        device.context->CopyResource(
            gradedOutput.staging.Get(), gradedOutput.texture.Get());
        CheckHresult(device.context->Map(gradedOutput.staging.Get(), 0,
            D3D11_MAP_READ, 0, &gradedMapped), label);
        gradedValues = static_cast<const Float4*>(gradedMapped.pData);
    }
    void VerifyGradeOnly()
    {
        gradeOnlyConstants = CreateConstantBuffer(
            *device.device.Get(), ToneMapConstants{
                0U, peak, kReferenceWhiteNits, uiWhite, 1.0F,
                kReferenceWhiteNits,
                static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(
                    fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, peak)),
                static_cast<float>(
                    fo4hdr::OperatorLutInputShaperMaximum(
                        fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt,
                        peak))});
        gradeTarget[0] = gradedOutput.rtv.Get();
        ReadGradeOutput(gradeOnlyConstants.Get(),
            "Map CreativeGradeLut grade-only pixel-shader output");
        gradedScene = ReadCreativeGradedScene(gradedValues, corpus);
        device.context->Unmap(gradedOutput.staging.Get(), 0);
        FO4HDR_WARP_CHECK(gradedScene[4].red > gradedScene[4].green);
        FO4HDR_WARP_CHECK(gradedScene[5].green > gradedScene[5].red);
        FO4HDR_WARP_CHECK(gradedScene[6].blue > gradedScene[6].red);
    }

    void VerifyZeroGrade()
    {
        // Zero grade strength must recover the original HDR scene exactly before
        // the selected operator. Zero saturation must preserve luminance without
        // introducing an SDR-range clamp.
        ungradedSettings = {
            0U, peak, kReferenceWhiteNits, uiWhite, 1.0F,
            kReferenceWhiteNits,
            static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(
                fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, peak)),
            static_cast<float>(fo4hdr::OperatorLutInputShaperMaximum(
                fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, peak))};
        ungradedSettings.vanillaGradeStrength = 0.0F;
        ungradedConstants = CreateConstantBuffer(
            *device.device.Get(), ungradedSettings);
        ReadGradeOutput(ungradedConstants.Get(),
            "Map CreativeGradeLut zero-grade output");
        VerifyCreativeUngradedPixels(gradedValues, scenePixels);
        device.context->Unmap(gradedOutput.staging.Get(), 0);
    }

    void VerifyHalfGrade()
    {
        auto halfGradeSettings = ungradedSettings;
        halfGradeSettings.vanillaGradeStrength = 0.5F;
        VerifyAdjustedGrade(halfGradeSettings, halfGradeConstants,
            "Map CreativeGradeLut half-grade output", [&] {
                VerifyCreativeHalfGradePixels(gradedValues, scenePixels, gradedScene);
            });
    }

    void VerifyDesaturation()
    {
        auto desaturatedSettings = ungradedSettings;
        desaturatedSettings.saturation = 0.0F;
        VerifyAdjustedGrade(desaturatedSettings, desaturatedConstants,
            "Map CreativeGradeLut zero-saturation output", [&] {
                VerifyCreativeDesaturatedPixels(gradedValues);
            });
    }

    void VerifyOversaturation()
    {
        auto oversaturatedSettings = ungradedSettings;
        oversaturatedSettings.saturation = 2.0F;
        VerifyAdjustedGrade(oversaturatedSettings, oversaturatedConstants,
            "Map CreativeGradeLut oversaturation output", [&] {
                VerifyCreativeOversaturatedPixels(gradedValues, scenePixels);
            });
    }

    template <class VerifyPixels>
    void VerifyAdjustedGrade(const ToneMapConstants& settings,
        ComPtr<ID3D11Buffer>& retainedConstants, const char* label,
        VerifyPixels verifyPixels)
    {
        retainedConstants = CreateConstantBuffer(*device.device.Get(), settings);
        ReadGradeOutput(retainedConstants.Get(), label);
        verifyPixels();
        device.context->Unmap(gradedOutput.staging.Get(), 0);
    }

    void VerifyAdaptation()
    {
        auto adaptedSettings = ungradedSettings;
        adaptedSettings.colorAdaptationRow0 = {0.0F, 0.0F, 1.0F, 0.0F};
        adaptedSettings.colorAdaptationRow2 = {1.0F, 0.0F, 0.0F, 0.0F};
        adaptedConstants = CreateConstantBuffer(
            *device.device.Get(), adaptedSettings);
        ReadGradeOutput(adaptedConstants.Get(),
            "Map CreativeGradeLut explicit-adaptation output");
        FO4HDR_WARP_CHECK(gradedValues[4].blue > gradedValues[4].red);
        FO4HDR_WARP_CHECK(gradedValues[6].red > gradedValues[6].blue);
        device.context->Unmap(gradedOutput.staging.Get(), 0);
    }

    void VerifySelectiveColor()
    {
        // Sample the shipping helper at mixed settings and geometric boundaries.
        // CPU reference inputs must match FP16 texture storage before comparison.
        const auto selectiveCases = CreativeSelectiveCases();

        const auto selectiveInputs = CreativeSelectiveInputs(corpus);

        CreativeSelectiveReport selectiveReport{};
        for (std::size_t settingIndex = 0;
             settingIndex < selectiveCases.size(); ++settingIndex)
        {
            fo4hdr::SelectiveColorPrepared selectivePrepared{};
            FO4HDR_WARP_CHECK(fo4hdr::PrepareSelectiveColor(
                selectiveCases[settingIndex], selectivePrepared));
            auto selectiveConstants = ungradedSettings;
            selectiveConstants.selectiveColorActive =
                selectivePrepared.effectiveActive ? 1U : 0U;
            for (std::size_t index = 0; index < selectivePrepared.regions.size(); ++index)
                selectiveConstants.selectiveColorRegions[index] = {
                    static_cast<float>(selectivePrepared.regions[index][0]),
                    static_cast<float>(selectivePrepared.regions[index][1]),
                    static_cast<float>(selectivePrepared.regions[index][2]), 0.0F};
            selectiveConstants.selectiveColorPlaneA = {
                static_cast<float>(selectivePrepared.planeA[0]),
                static_cast<float>(selectivePrepared.planeA[1]),
                static_cast<float>(selectivePrepared.planeA[2]), 0.0F};
            selectiveConstants.selectiveColorPlaneB = {
                static_cast<float>(selectivePrepared.planeB[0]),
                static_cast<float>(selectivePrepared.planeB[1]),
                static_cast<float>(selectivePrepared.planeB[2]), 0.0F};
            const auto selectiveBuffer = CreateConstantBuffer(
                *device.device.Get(), selectiveConstants);
            for (std::size_t batch = 0; batch < selectiveInputs.size(); ++batch)
            {
                const auto pixels = MakeFp16Pixels(selectiveInputs[batch]);
                const D3D11_SUBRESOURCE_DATA inputData{
                    pixels.data(), static_cast<UINT>(sizeof(pixels)), 0};
                const auto selectiveScene = CreateTexture(*device.device.Get(),
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    D3D11_BIND_SHADER_RESOURCE, &inputData);
                resources[0] = selectiveScene.srv.Get();
                DrawFullscreen(*device.context.Get(), { vertexShader, shader.Get(),
                    lutConstants.Get(), selectiveBuffer.Get(), resources.data(),
                    static_cast<UINT>(resources.size()), samplers.data(),
                    static_cast<UINT>(samplers.size()), gradeTarget, 1});
                device.context->CopyResource(
                    gradedOutput.staging.Get(), gradedOutput.texture.Get());
                CheckHresult(device.context->Map(gradedOutput.staging.Get(), 0,
                    D3D11_MAP_READ, 0, &gradedMapped),
                    "Map CreativeGradeLut selective-color output");
                gradedValues = static_cast<const Float4*>(gradedMapped.pData);
                selectiveReport.settingIndex = settingIndex;
                selectiveReport.batch = batch;
                VerifyCreativeSelectiveBatch(
                    pixels, gradedValues, selectivePrepared, selectiveReport);
                device.context->Unmap(gradedOutput.staging.Get(), 0);
            }
        }
        resources[0] = scene.srv.Get();
        std::cout << "Selective Color: 160 shipping samples, max normalized error="
            << selectiveReport.maximumNormalizedError << ", max FP16 ULP="
            << selectiveReport.maximumHalfDistance << '\n';
    }

    void VerifyOperators()
    {
        for (std::size_t modeIndex = 0;
             modeIndex < toneMappers.size(); ++modeIndex) {
            const auto productionAlgorithm =
                ProductionAlgorithm(toneMappers[modeIndex]);
            Texture3D operatorLut = CreateOperatorLut(
                *device.device.Get(), productionAlgorithm, peak);
            resources[7] = operatorLut.srv.Get();
            resources[8] = nullptr;
            samplers[7] = lutSampler.Get();
            const auto toneMapConstants = CreateConstantBuffer(
                *device.device.Get(), ToneMapConstants{
                    ProductionMode(toneMappers[modeIndex]), peak,
                    kReferenceWhiteNits, uiWhite, 1.0F,
                    kReferenceWhiteNits,
                    static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(
                        productionAlgorithm, peak)),
                    static_cast<float>(fo4hdr::OperatorLutInputShaperMaximum(
                        productionAlgorithm, peak))});
            DrawFullscreen(
                *device.context.Get(), { vertexShader, shader.Get(),
                lutConstants.Get(), toneMapConstants.Get(), resources.data(),
                static_cast<UINT>(resources.size()), samplers.data(),
                static_cast<UINT>(samplers.size()), target, 1});

            device.context->CopyResource(
                output.staging.Get(), output.texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CheckHresult(device.context->Map(
                output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                "Map CreativeGradeLut shipping pixel-shader output");
            const auto* values = static_cast<
                const std::array<std::uint16_t, 4>*>(mapped.pData);
            VerifyCreativeOperatorPixels(values, {
                toneMappers[modeIndex], algorithmNames[modeIndex], peak, uiWhite,
                gradedScene, scenePixels});
            device.context->Unmap(output.staging.Get(), 0);
        }
    }

};

void VerifyCreativeGradeLutPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sceneSampler)
{
    CreativeGradeFixture fixture(device, vertexShader, sceneSampler);
    fixture.Verify();
}

}  // namespace fo4hdr::tests::gpu
