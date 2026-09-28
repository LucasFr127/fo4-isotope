// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "ShaderTestSuite.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <string>
#include <string_view>

namespace fo4hdr::tests::gpu {

void VerifyShippingPixelShaders(Device& device)
{
    const auto vertexShader = CreateVertexShader(
        *device.device.Get(), fo4hdr::generated::test::kFullscreenVs);
    const auto sampler = CreateClampPointSampler(*device.device.Get());
    VerifyDeveloperExposureLock(device, vertexShader.Get(), sampler.Get());
    VerifyNativeGradeColorChain(device, vertexShader.Get(), sampler.Get());
    VerifyImageControls(device, vertexShader.Get(), sampler.Get());
    VerifyRadialVanillaGradeRemoval(
        device, vertexShader.Get(), sampler.Get());
    VerifyLutProfileGradeRemoval(
        device, vertexShader.Get(), sampler.Get());
    VerifyHdrGradeTransfer(device, vertexShader.Get(), sampler.Get());
    VerifyNativeReferenceGrade(device, vertexShader.Get(), sampler.Get());
    VerifyBloomStrengthPixelShader(
        device, vertexShader.Get(), sampler.Get());
    VerifyVideoPixelShader(device, vertexShader.Get(), sampler.Get());
    VerifyFxaaWideGamutPixelShader(
        device, vertexShader.Get(), sampler.Get());
    VerifyCreativeGradeLutPixelShader(
        device, vertexShader.Get(), sampler.Get());
    const auto temporalAa = CreatePixelShader(
        *device.device.Get(), fo4hdr::generated::kTemporalAa);
    VerifyTemporalWideGamutPixelShader(device, { vertexShader.Get(), sampler.Get(), temporalAa.Get(), false,
        "TemporalAa"});
    const auto temporalAa2 = CreatePixelShader(
        *device.device.Get(), fo4hdr::generated::kTemporalAa2);
    VerifyTemporalWideGamutPixelShader(device, { vertexShader.Get(), sampler.Get(), temporalAa2.Get(), true,
        "TemporalAa2"});
}

void VerifyLutGradeProfiles(Device& device)
{
    const auto vertexShader = CreateVertexShader(
        *device.device.Get(), fo4hdr::generated::test::kFullscreenVs);
    const auto sampler = CreateClampPointSampler(*device.device.Get());
    VerifyLutProfileGradeRemoval(
        device, vertexShader.Get(), sampler.Get());
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

void CheckVideoPixels(const std::array<std::uint16_t, 4>* values,
    const auto& inputPixels, double scale)
{
    for (std::size_t index = 0; index < kWidth; ++index) {
        const auto bt2020 = fo4hdr::ConvertBt709ToBt2020({
            DecodeSrgb(Binary16ToFloat(inputPixels[index][2])),
            DecodeSrgb(Binary16ToFloat(inputPixels[index][1])),
            DecodeSrgb(Binary16ToFloat(inputPixels[index][0]))});
        const std::array expected{
            bt2020.red * scale, bt2020.green * scale,
            bt2020.blue * scale};
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            const auto expectedHalf = FloatToBinary16(
                static_cast<float>(expected[channel]));
            const auto actual = Binary16ToFloat(values[index][channel]);
            if (CodeDistance(values[index][channel], expectedHalf) > 1U) {
                std::cerr << "Video BT.2020 parity: sample=" << index
                          << " channel=" << channel << " actual=" << actual
                          << " expected=" << expected[channel] << '\n';
            }
            FO4HDR_WARP_CHECK(std::isfinite(actual));
            FO4HDR_WARP_CHECK(actual >= 0.0F);
            FO4HDR_WARP_CHECK(
                CodeDistance(values[index][channel], expectedHalf) <= 1U);
        }
        FO4HDR_WARP_CHECK(
            CodeDistance(values[index][3], inputPixels[index][3]) == 0U);
    }
}

struct BloomDiagnostics
{
    const char* variant;
    float strength;
};

void CheckBloomPixels(const Float4* values, const auto& expected, BloomDiagnostics diagnostics)
{
    for (std::size_t index = 0; index < kWidth; ++index) {
        const std::array actual{
            values[index].red, values[index].green,
            values[index].blue};
        for (std::size_t channel = 0; channel < actual.size();
             ++channel) {
            const bool acceptable = std::isfinite(actual[channel]) &&
                std::abs(actual[channel] - expected[channel]) <=
                    0.002F;
            if (!acceptable) {
                std::cerr << "Bloom Strength "
                          << diagnostics.variant
                          << ": strength=" << diagnostics.strength
                          << " sample=" << index
                          << " channel=" << channel
                          << " actual=" << actual[channel]
                          << " expected=" << expected[channel]
                          << '\n';
            }
            FO4HDR_WARP_CHECK(acceptable);
        }
    }
}

std::array<ComPtr<ID3D11PixelShader>, 4> CreateBloomVariants(ID3D11Device& device)
{
    std::array<ComPtr<ID3D11PixelShader>, 4> shaders{
        CreatePixelShader(
            device, fo4hdr::generated::kToneMapA),
        CreatePixelShader(
            device, fo4hdr::generated::kToneMapB),
        CreatePixelShader(
            device, fo4hdr::generated::kToneMapNewA),
        CreatePixelShader(
            device, fo4hdr::generated::kToneMapNewB)};
    return shaders;
}

void VerifyVideoPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler)
{
    constexpr std::array<Float4, kWidth> encodedBgr{{
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.01F, 0.02F, 0.04F, 0.125F},
        {0.04045F, 0.18F, 0.5F, 0.25F},
        {0.25F, 0.5F, 0.75F, 0.375F},
        {0.8F, 0.4F, 0.1F, 0.5F},
        {1.0F, 0.0F, 0.0F, 0.625F},
        {0.0F, 1.0F, 0.0F, 0.75F},
        {0.0F, 0.0F, 1.0F, 1.0F},
    }};
    const auto inputPixels = MakeFp16Pixels(encodedBgr);
    D3D11_SUBRESOURCE_DATA inputData{
        inputPixels.data(), static_cast<UINT>(sizeof(inputPixels)), 0};
    auto input = CreateTexture(
        *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE, &inputData);
    auto output = CreateTexture(
        *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET);
    const auto shader = CreatePixelShader(
        *device.device.Get(), fo4hdr::generated::kVideo);
    constexpr float videoWhiteNits = 100.0F;
    const auto toneMapConstants = CreateConstantBuffer(
        *device.device.Get(), ToneMapConstants{
            0U, 1000.0F, kReferenceWhiteNits, kReferenceWhiteNits,
            1.0F, videoWhiteNits,
            static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(
                fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, 1000.0)),
            static_cast<float>(
                fo4hdr::OperatorLutInputShaperMaximum(
                    fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt,
                    1000.0))});
    ID3D11ShaderResourceView* resources[]{input.srv.Get()};
    ID3D11SamplerState* samplers[]{sampler};
    ID3D11RenderTargetView* targets[]{output.rtv.Get()};
    DrawFullscreen(
        *device.context.Get(), { vertexShader, shader.Get(), nullptr,
        toneMapConstants.Get(), resources, 1, samplers, 1, targets, 1});

    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(
        output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map Video shipping pixel-shader output");
    const auto* values = static_cast<
        const std::array<std::uint16_t, 4>*>(mapped.pData);
    const auto scale = static_cast<double>(videoWhiteNits) /
        static_cast<double>(kReferenceWhiteNits);
    CheckVideoPixels(values, inputPixels, scale);
    device.context->Unmap(output.staging.Get(), 0);
}

struct BloomConstants
{
    ComPtr<ID3D11Buffer> original;
    ComPtr<ID3D11Buffer> affine;
};
BloomConstants CreateBloomConstants(ID3D11Device& device)
{
    ImageSpaceConstants imageSpace{};
    imageSpace.values[1] = {100.0F, 0.0F, 1.001F, 0.0F};
    imageSpace.values[2] = {1.0F, 0.0F, 1.0F, 1.0F};
    imageSpace.values[3] = {0.0F, 0.0F, 0.0F, 0.0F};
    imageSpace.values[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    auto imageSpaceConstants = CreateConstantBuffer(
        device, imageSpace);
    ImageSpaceConstantsWithFinalAffine affineImageSpace{};
    std::copy(imageSpace.values.begin(), imageSpace.values.end(),
        affineImageSpace.values.begin());
    auto affineImageSpaceConstants = CreateConstantBuffer(
        device, affineImageSpace);
    return {std::move(imageSpaceConstants), std::move(affineImageSpaceConstants)};
}

void ReadBloomOutput(Device& device, Texture& output, BloomDiagnostics diagnostics,
    const Float4& bloomColor)
{
    const auto strength = diagnostics.strength;
    device.context->CopyResource(
        output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(output.staging.Get(), 0,
        D3D11_MAP_READ, 0, &mapped),
        "Map Bloom Strength shipping pixel-shader output");
    const auto* values = static_cast<const Float4*>(mapped.pData);
    const std::array expected{
        bloomColor.red * strength,
        bloomColor.green * strength,
        bloomColor.blue * strength};
    CheckBloomPixels(values, expected, {diagnostics.variant, strength});
    device.context->Unmap(output.staging.Get(), 0);
}

void VerifyBloomStrengthPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler)
{
    constexpr Float4 bloomColor{0.25F, 0.5F, 1.0F, 1.0F};
    const auto bloomPixels = MakeFp16Pixels(UniformPixels(bloomColor));
    const auto blackPixels = MakeFp16Pixels(UniformPixels({}));
    const auto exposurePixels = MakeFp16Pixels(
        UniformPixels({1.0F, 1.0F, 1.0F, 1.0F}));
    D3D11_SUBRESOURCE_DATA bloomData{
        bloomPixels.data(), static_cast<UINT>(sizeof(bloomPixels)), 0};
    D3D11_SUBRESOURCE_DATA blackData{
        blackPixels.data(), static_cast<UINT>(sizeof(blackPixels)), 0};
    D3D11_SUBRESOURCE_DATA exposureData{
        exposurePixels.data(), static_cast<UINT>(sizeof(exposurePixels)), 0};
    auto bloom = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
        &bloomData);
    auto scene = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
        &blackData);
    auto exposure = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
        &exposureData);
    auto depth = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
        &blackData);
    auto output = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto shaders = CreateBloomVariants(*device.device.Get());
    constexpr std::array variantNames{
        "ToneMapA", "ToneMapB", "ToneMapNewA", "ToneMapNewB"};

    const auto buffers = CreateBloomConstants(*device.device.Get());
    const auto& [imageSpaceConstants, affineImageSpaceConstants] = buffers;
    std::array<ID3D11ShaderResourceView*, 4> originalResources{
        bloom.srv.Get(), scene.srv.Get(), exposure.srv.Get(),
        depth.srv.Get()};
    std::array<ID3D11ShaderResourceView*, 4> newResources{
        scene.srv.Get(), bloom.srv.Get(), exposure.srv.Get(),
        depth.srv.Get()};
    std::array<ID3D11SamplerState*, 4> samplers{
        sampler, sampler, sampler, sampler};
    ID3D11RenderTargetView* targets[]{output.rtv.Get()};

    for (std::size_t variant = 0; variant < shaders.size(); ++variant) {
        const bool newVariant = variant >= 2;
        const bool finalAffine = (variant & 1U) != 0U;
        auto& resources = newVariant ? newResources : originalResources;
        auto* constants = finalAffine
            ? affineImageSpaceConstants.Get()
            : imageSpaceConstants.Get();
        for (const float strength : {0.0F, 1.0F, 2.0F}) {
            ToneMapConstants settings{
                0U, 1000.0F, kReferenceWhiteNits, kReferenceWhiteNits,
                1.0F, kReferenceWhiteNits, 0.0F, 0.0F};
            settings.bloomStrength = strength;
            const auto settingsBuffer = CreateConstantBuffer(
                *device.device.Get(), settings);
            DrawFullscreen(*device.context.Get(), { vertexShader,
                shaders[variant].Get(), constants, settingsBuffer.Get(),
                resources.data(), static_cast<UINT>(resources.size()),
                samplers.data(), static_cast<UINT>(samplers.size()),
                targets, 1});

            ReadBloomOutput(device, output, {variantNames[variant], strength}, bloomColor);
        }
    }
}

void VerifyFxaaWideGamutPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler)
{
    constexpr Float4 wideBt2020{4.0F, 0.25F, 0.05F, 1.0F};
    const auto samples = UniformPixels(wideBt2020);
    const auto inputPixels = MakeFp16Pixels(samples);
    D3D11_SUBRESOURCE_DATA inputData{
        inputPixels.data(), static_cast<UINT>(sizeof(inputPixels)), 0};
    auto input = CreateTexture(
        *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE, &inputData);
    auto output = CreateTexture(
        *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET);
    const auto shader = CreatePixelShader(
        *device.device.Get(), fo4hdr::generated::kFxaa);
    const auto constants = CreateConstantBuffer(
        *device.device.Get(), FxaaConstants{{
            1.0F / static_cast<float>(kWidth),
            1.0F / static_cast<float>(kHeight), 0.0F, 0.0F}});
    ID3D11ShaderResourceView* resources[]{input.srv.Get()};
    ID3D11SamplerState* samplers[]{sampler};
    ID3D11RenderTargetView* targets[]{output.rtv.Get()};
    DrawFullscreen(
        *device.context.Get(), { vertexShader, shader.Get(), constants.Get(),
        nullptr, resources, 1, samplers, 1, targets, 1});

    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(
        output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map FXAA wide-gamut shipping pixel-shader output");
    const auto* values = static_cast<
        const std::array<std::uint16_t, 4>*>(mapped.pData);
    for (std::size_t index = 0; index < kWidth; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto actual = Binary16ToFloat(values[index][channel]);
            FO4HDR_WARP_CHECK(std::isfinite(actual));
            FO4HDR_WARP_CHECK(actual >= 0.0F);
            FO4HDR_WARP_CHECK(
                CodeDistance(values[index][channel],
                    inputPixels[index][channel]) <= 1U);
        }
        const auto bt709 = fo4hdr::ConvertBt2020ToBt709ForComparison({
            Binary16ToFloat(values[index][0]),
            Binary16ToFloat(values[index][1]),
            Binary16ToFloat(values[index][2])});
        FO4HDR_WARP_CHECK(bt709.green < 0.0);
    }
    device.context->Unmap(output.staging.Get(), 0);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

struct TemporalScenario : TemporalVariant
{
    Device& device;
    static constexpr Float4 wideBt2020{4.0F, 0.25F, 0.05F, 1.0F};
    std::array<std::array<std::uint16_t, 4>, kWidth> scenePixels{};
    Texture scene, zero, depth, metadata, filtered;
    TemporalConstants temporalValues{};
    ComPtr<ID3D11Buffer> temporalConstants, toneMapConstants;
    std::array<ID3D11ShaderResourceView*, 5> resources{};
    std::array<ID3D11SamplerState*, 5> samplers{};
    std::array<ID3D11RenderTargetView*, 2> targets{};
    UINT resourceCount{}, samplerCount{};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    std::array<std::array<std::uint16_t, 4>, kWidth> resetPixels{};
    Texture resetScene, hostileMetadata, hostileDepth, adaptedMetadata;
    ToneMapConstants resetSettings{};
    ComPtr<ID3D11Buffer> hostileTemporalConstants;

    TemporalScenario(Device& source, TemporalVariant variant) : TemporalVariant(variant), device(source) {}
    void InitializeTextures()
    {
        const auto sceneSamples = UniformPixels(wideBt2020);
        scenePixels = MakeFp16Pixels(sceneSamples);
        const auto zeroPixels = MakeFp16Pixels(UniformPixels({}));
        const auto depthPixels = MakeFp16Pixels(
            UniformPixels({1.0F, 1.0F, 1.0F, 1.0F}));
        D3D11_SUBRESOURCE_DATA sceneData{
            scenePixels.data(), static_cast<UINT>(sizeof(scenePixels)), 0};
        D3D11_SUBRESOURCE_DATA zeroData{
            zeroPixels.data(), static_cast<UINT>(sizeof(zeroPixels)), 0};
        D3D11_SUBRESOURCE_DATA depthData{
            depthPixels.data(), static_cast<UINT>(sizeof(depthPixels)), 0};
        scene = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &sceneData);
        zero = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &zeroData);
        depth = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &depthData);
        metadata = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        filtered = CreateTexture(
            *device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET);

    }

    void InitializeConstants()
    {
        temporalValues.values = {{
            {1.0F / 128.0F, 0.0F,
                static_cast<float>(kWidth), static_cast<float>(kHeight)},
            {0.0F, 0.0F, 0.0F, 0.0F},
            {1.0F, 0.0F, 0.0F, 0.0F},
            {1.0F / static_cast<float>(kWidth),
                1.0F / static_cast<float>(kHeight), 0.0F, 0.0F},
            {1.0F, 1.0F, 0.0F, 1.0F},
            {0.0F, 0.0F,
                static_cast<float>(kWidth), static_cast<float>(kHeight)},
        }};
        temporalConstants = CreateConstantBuffer(
            *device.device.Get(), temporalValues);
        toneMapConstants = CreateConstantBuffer(
            *device.device.Get(), ToneMapConstants{
                1U, 1000.0F, kReferenceWhiteNits, 300.0F,
                1.0F, kReferenceWhiteNits,
                static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(
                    fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, 1000.0)),
                static_cast<float>(
                    fo4hdr::OperatorLutInputShaperMaximum(
                        fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt,
                        1000.0))});
        resources = {
            scene.srv.Get(), zero.srv.Get(), zero.srv.Get(), depth.srv.Get(),
            zero.srv.Get()};
        samplers = {
            sampler, sampler, sampler, sampler, sampler};
        targets = {
            metadata.rtv.Get(), filtered.rtv.Get()};
    }

    void CheckUniform()
    {
        constexpr float metadataSentinel[4]{1.0F, 1.0F, 1.0F, 1.0F};
        device.context->ClearRenderTargetView(
            metadata.rtv.Get(), metadataSentinel);
        resourceCount = secondVariant ? 5U : 4U;
        samplerCount = secondVariant ? 5U : 4U;
        DrawFullscreen(
            *device.context.Get(), { vertexShader, shader,
            temporalConstants.Get(), toneMapConstants.Get(), resources.data(),
            resourceCount, samplers.data(), samplerCount, targets.data(),
            static_cast<UINT>(targets.size())});

        device.context->CopyResource(
            filtered.staging.Get(), filtered.texture.Get());
        CheckHresult(device.context->Map(
            filtered.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            std::string("Map ") + std::string(variantName) +
                " wide-gamut shipping pixel-shader output");
        const auto* values = static_cast<
            const std::array<std::uint16_t, 4>*>(mapped.pData);
        for (std::size_t index = 0; index < kWidth; ++index) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto actual = Binary16ToFloat(values[index][channel]);
                const auto expected = Binary16ToFloat(scenePixels[index][channel]);
                const auto tolerance = std::max(0.002F, expected * 0.005F);
                if (std::abs(actual - expected) > tolerance) {
                    std::cerr << "" << variantName
                              << " uniform BT.2020 transport: sample=" << index
                              << " channel=" << channel << " actual=" << actual
                              << " expected=" << expected << '\n';
                }
                FO4HDR_WARP_CHECK(std::isfinite(actual));
                FO4HDR_WARP_CHECK(actual >= 0.0F);
                FO4HDR_WARP_CHECK(std::abs(actual - expected) <= tolerance);
            }
            FO4HDR_WARP_CHECK(Binary16ToFloat(values[index][3]) == 1.0F);
            const auto bt709 = fo4hdr::ConvertBt2020ToBt709ForComparison({
                Binary16ToFloat(values[index][0]),
                Binary16ToFloat(values[index][1]),
                Binary16ToFloat(values[index][2])});
            FO4HDR_WARP_CHECK(bt709.green < 0.0);
        }
        device.context->Unmap(filtered.staging.Get(), 0);

        device.context->CopyResource(
            metadata.staging.Get(), metadata.texture.Get());
        CheckHresult(device.context->Map(
            metadata.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            std::string("Map ") + std::string(variantName) +
                " metadata MRT output");
        const auto* metadataValues = static_cast<const std::uint8_t*>(mapped.pData);
        bool metadataWritten{};
        for (std::size_t index = 0; index < kWidth * 4U; ++index)
            metadataWritten = metadataWritten || metadataValues[index] != 255U;
        FO4HDR_WARP_CHECK(metadataWritten);
        device.context->Unmap(metadata.staging.Get(), 0);

    }

    void InitializeHistory()
    {
        // Live settings do not own Fallout 4's temporal history. Exercise a large
        // current-frame signal change against hostile but valid prior metadata,
        // check that the reserved b13 lane cannot alter either MRT, then let the native
        // algorithm adapt its own metadata on the following frame.
        resetPixels = MakeFp16Pixels(kSamples);
        D3D11_SUBRESOURCE_DATA resetData{
            resetPixels.data(), static_cast<UINT>(sizeof(resetPixels)), 0};
        resetScene = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            &resetData);
        const auto hostileMetadataPixels = MakeUnorm8Pixels(
            UniformPixels({0.95F, 0.85F, 0.75F, 0.5F}));
        constexpr std::array<Float4, kWidth> hostileDepthSamples{{
            {0.2F, 0.0F, 0.0F, 1.0F},
            {0.3F, 0.0F, 0.0F, 1.0F},
            {0.4F, 0.0F, 0.0F, 1.0F},
            {0.5F, 0.0F, 0.0F, 1.0F},
            {0.6F, 0.0F, 0.0F, 1.0F},
            {0.7F, 0.0F, 0.0F, 1.0F},
            {0.8F, 0.0F, 0.0F, 1.0F},
            {0.9F, 0.0F, 0.0F, 1.0F}}};
        const auto hostileDepthPixels = MakeFp16Pixels(hostileDepthSamples);
        D3D11_SUBRESOURCE_DATA hostileMetadataData{
            hostileMetadataPixels.data(),
            static_cast<UINT>(sizeof(hostileMetadataPixels)), 0};
        D3D11_SUBRESOURCE_DATA hostileDepthData{
            hostileDepthPixels.data(),
            static_cast<UINT>(sizeof(hostileDepthPixels)), 0};
        hostileMetadata = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
            &hostileMetadataData);
        hostileDepth = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            &hostileDepthData);
        adaptedMetadata = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        resources[0] = resetScene.srv.Get();
        resources[1] = hostileMetadata.srv.Get();
        resources[2] = zero.srv.Get();
        resources[3] = hostileDepth.srv.Get();
        if (secondVariant) resources[4] = zero.srv.Get();
        resetSettings = {
            1U, 1000.0F, kReferenceWhiteNits, 300.0F,
            1.0F, kReferenceWhiteNits,
            static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(
                fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, 1000.0)),
            static_cast<float>(fo4hdr::OperatorLutInputShaperMaximum(
                fo4hdr::ProductionToneMapperAlgorithm::aces2Lmt, 1000.0))};
        auto hostileTemporalValues = temporalValues;
        hostileTemporalValues.values[2] = {
            0.25F, 0.25F, 0.25F, 0.25F};
        hostileTemporalValues.values[4] = {1.0F, 1.0F, 1.0F, 0.0F};
        hostileTemporalConstants = CreateConstantBuffer(
            *device.device.Get(), hostileTemporalValues);
    }

    void CheckStaleOutput(const std::array<Float4, kWidth>& staleOutput)
    {
        bool staleHistoryAffectedOutput{};
        float maximumStaleDifference{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const std::array actual{
                staleOutput[index].red,
                staleOutput[index].green,
                staleOutput[index].blue};
            for (std::size_t channel = 0; channel < actual.size(); ++channel) {
                const auto expected = Binary16ToFloat(resetPixels[index][channel]);
                maximumStaleDifference = std::max(maximumStaleDifference,
                    std::abs(actual[channel] - expected));
                staleHistoryAffectedOutput = staleHistoryAffectedOutput ||
                    std::abs(actual[channel] - expected) >
                        std::max(0.004F, expected * 0.01F);
            }
        }
        if (!staleHistoryAffectedOutput)
            std::cerr << "" << variantName
                      << " stale history maximum difference="
                      << maximumStaleDifference << '\n';
        FO4HDR_WARP_CHECK(staleHistoryAffectedOutput);

    }
    void CheckReservedOutput(const std::array<Float4, kWidth>& reservedOutput,
        const std::array<Float4, kWidth>& staleOutput)
    {
        for (std::size_t index = 0; index < kWidth; ++index) {
            const std::array actual{
                reservedOutput[index].red,
                reservedOutput[index].green,
                reservedOutput[index].blue};
            const std::array expectedFiltered{
                staleOutput[index].red,
                staleOutput[index].green,
                staleOutput[index].blue};
            for (std::size_t channel = 0; channel < actual.size(); ++channel) {
                FO4HDR_WARP_CHECK(std::isfinite(actual[channel]));
                FO4HDR_WARP_CHECK(actual[channel] == expectedFiltered[channel]);
            }
        }
    }
    void CheckAdaptedOutput(const std::array<Float4, kWidth>& adaptedOutput,
        const std::array<Float4, kWidth>& repeatedAdaptedOutput,
        const std::array<Float4, kWidth>& staleOutput)
    {
        bool nativeMetadataAdaptedOutput{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const std::array actual{
                adaptedOutput[index].red,
                adaptedOutput[index].green,
                adaptedOutput[index].blue};
            const std::array repeated{
                repeatedAdaptedOutput[index].red,
                repeatedAdaptedOutput[index].green,
                repeatedAdaptedOutput[index].blue};
            const std::array stale{
                staleOutput[index].red,
                staleOutput[index].green,
                staleOutput[index].blue};
            for (std::size_t channel = 0; channel < actual.size(); ++channel) {
                FO4HDR_WARP_CHECK(std::isfinite(actual[channel]));
                FO4HDR_WARP_CHECK(actual[channel] >= 0.0F);
                FO4HDR_WARP_CHECK(
                    actual[channel] == repeated[channel]);
                nativeMetadataAdaptedOutput =
                    nativeMetadataAdaptedOutput ||
                    std::abs(actual[channel] - stale[channel]) > 0.002F;
            }
        }
        FO4HDR_WARP_CHECK(nativeMetadataAdaptedOutput);
    }
    void CheckHistory()
    {
        const auto staleOutput = DrawTemporal(0U, "stale-history");
        const auto reservedZeroMetadata = ReadMetadata("reserved-zero");
        device.context->CopyResource(
            adaptedMetadata.texture.Get(), metadata.texture.Get());
        CheckStaleOutput(staleOutput);
        const auto reservedOutput = DrawTemporal(1U, "reserved-one");
        const auto reservedOneMetadata = ReadMetadata("reserved-one");
        CheckReservedOutput(reservedOutput, staleOutput);
        FO4HDR_WARP_CHECK(reservedZeroMetadata == reservedOneMetadata);

        resources[1] = adaptedMetadata.srv.Get();
        const auto adaptedOutput = DrawTemporal(0U, "native-adaptation");
        const auto repeatedAdaptedOutput = DrawTemporal(
            0U, "native-adaptation-repeat");
        CheckAdaptedOutput(adaptedOutput, repeatedAdaptedOutput, staleOutput);
    }
    std::array<Float4, kWidth> DrawTemporal(std::uint32_t reservedValue, std::string_view phase)
    {
        auto settings = resetSettings;
        settings.liveSettingsReserved = reservedValue;
        const auto settingsConstants = CreateConstantBuffer(
            *device.device.Get(), settings);
        DrawFullscreen(*device.context.Get(), { vertexShader, shader,
            hostileTemporalConstants.Get(), settingsConstants.Get(),
            resources.data(), resourceCount, samplers.data(), samplerCount,
            targets.data(), static_cast<UINT>(targets.size())});
        device.context->CopyResource(
            filtered.staging.Get(), filtered.texture.Get());
        CheckHresult(device.context->Map(filtered.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            std::string("Map ") + std::string(variantName) + " " +
                std::string(phase) + " temporal output");
        const auto* outputValues = static_cast<const
            std::array<std::uint16_t, 4>*>(mapped.pData);
        std::array<Float4, kWidth> result{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            result[index] = {
                Binary16ToFloat(outputValues[index][0]),
                Binary16ToFloat(outputValues[index][1]),
                Binary16ToFloat(outputValues[index][2]),
                Binary16ToFloat(outputValues[index][3])};
        }
        device.context->Unmap(filtered.staging.Get(), 0);
        return result;
    }
    std::array<std::uint8_t, kWidth * 4U> ReadMetadata(std::string_view phase)
    {
        device.context->CopyResource(
            metadata.staging.Get(), metadata.texture.Get());
        CheckHresult(device.context->Map(metadata.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            std::string("Map ") + std::string(variantName) + " " +
                std::string(phase) + " temporal metadata");
        const auto* values = static_cast<const std::uint8_t*>(mapped.pData);
        std::array<std::uint8_t, kWidth * 4U> result{};
        std::copy_n(values, result.size(), result.begin());
        device.context->Unmap(metadata.staging.Get(), 0);
        return result;
    }
    void Run()
    {
        InitializeTextures();
        InitializeConstants();
        CheckUniform();
        InitializeHistory();
        CheckHistory();
    }
};

void VerifyTemporalWideGamutPixelShader(Device& device, TemporalVariant variant)
{
    TemporalScenario scenario(device, variant);
    scenario.Run();
}

}  // namespace fo4hdr::tests::gpu
