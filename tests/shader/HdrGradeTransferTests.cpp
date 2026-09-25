// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "Fallout4HDR/ShippingShaders.generated.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "reference/NativeGradeReference.hpp"
#include "reference/ImageControlReference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <vector>

namespace fo4hdr::tests::gpu {

using HdrGradeRgb = NativeGradeRgb;

[[nodiscard]] double HdrGradeLuminance(const HdrGradeRgb& rgb)
{
    return rgb[0] * 0.212500006 + rgb[1] * 0.715399981 + rgb[2] * 0.0720999986;
}

struct HdrGradeTransferCase { HdrGradeRgb scene, codes; };

std::vector<HdrGradeTransferCase> HdrGradeTransferCases(double nan, double inf)
{
    std::vector<HdrGradeTransferCase> cases{
        {{}, {}}, {{}, {0.08, 0.08, 0.08}},
        {{16, 4, 1}, {}}, {{16, 4, 1}, {1, 0.5, 0.25}},
        {{5.599, 1, 0.00001}, {0.7, 0.4, 0.1}},
        {{5.6, 1, 0.00001}, {0.7, 0.4, 0.1}},
        {{5.601, 1, 0.00001}, {0.7, 0.4, 0.1}},
        {{65504, 0.00006103515625, 4}, {1, 0.001, 0.2}},
        {{65504, 32752, 16376}, {2, 1, 0.5}},
        {{nan, inf, -inf}, {nan, -1, inf}},
        {{-1, 0, 0.18}, {0.2, 0.4, 0.7}},
        {{65504, 65504, 65504}, {65504, 32752, 16376}}
    };
    return cases;
}

struct HdrGradeTransferSample
{
    double strength;
    std::size_t index;
    std::size_t identityStart;
};

void CheckHdrGradeTransferMath(const Float4& pixel,
    const HdrGradeTransferCase& test, const HdrGradeTransferSample& sample)
{
    const HdrGradeRgb actual{pixel.red, pixel.green, pixel.blue};
    const auto expected = DecodeAndTransferNativeGrade(test.scene, test.codes, sample.strength);
    const double tolerance = 3.0e-5 * std::max({expected[0], expected[1], expected[2], 1.0e-12});
    for (std::size_t c = 0; c < 3; ++c) {
        const bool parity = std::abs(actual[c] - expected[c]) <= tolerance;
        if (!parity) std::cerr << "Radial grading case=" << sample.index << " strength=" << sample.strength
            << " channel=" << c << " expected=" << expected[c] << " actual=" << actual[c] << '\n';
        FO4HDR_WARP_CHECK(parity);
        FO4HDR_WARP_CHECK(std::isfinite(actual[c]) && actual[c] >= 0 && actual[c] <= 65504);
        if (!(sample.strength > 0)) FO4HDR_WARP_CHECK(actual[c] == NativeGradeClean(test.scene[c]));
        if (sample.index >= sample.identityStart)
            FO4HDR_WARP_CHECK(std::abs(actual[c] - test.scene[c]) <=
                3.0e-5 * std::max(test.scene[c], 1.0e-12));
    }
}

void VerifyHdrGradeTransferMath(Device& device)
{
    const double nan = std::numeric_limits<float>::quiet_NaN();
    const double inf = std::numeric_limits<float>::infinity();
    auto cases = HdrGradeTransferCases(nan, inf);
    const auto identityStart = cases.size();
    for (const HdrGradeRgb source : {HdrGradeRgb{0, 0, 0},
        HdrGradeRgb{0.18, 0.18, 0.18}, HdrGradeRgb{65504, 0.00006103515625, 4}}) {
        const double scale = NativeGradeScale(*std::max_element(source.begin(), source.end()));
        HdrGradeRgb codes{};
        for (std::size_t c = 0; c < 3; ++c) codes[c] = std::pow(source[c] * scale, 1.0 / 2.2);
        cases.push_back({source, codes});
    }
    const auto shader = CreateShader(*device.device.Get(),
        fo4hdr::generated::test::kHdrGradeTransferCs);
    auto output = CreateSizedTexture(*device.device.Get(), { 1, 1,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS});
    for (std::size_t index = 0; index < cases.size(); ++index)
    for (const double strength : {-inf, -1.0, 0.0, 0.5, 1.0, 2.0, inf, nan}) {
        auto test = cases[index];
        const auto create = [&](HdrGradeRgb& rgb, double alpha) {
            Float4 pixel{static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                static_cast<float>(rgb[2]), static_cast<float>(alpha)};
            // Match the representable GPU input before evaluating the oracle.
            rgb = {pixel.red, pixel.green, pixel.blue};
            D3D11_SUBRESOURCE_DATA data{&pixel, sizeof(Float4), 0};
            return CreateSizedTexture(*device.device.Get(), { 1, 1,
                DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data});
        };
        auto scene = create(test.scene, strength);
        auto codes = create(test.codes, 1);
        std::array<ID3D11ShaderResourceView*, 3> inputs{scene.srv.Get(), nullptr, codes.srv.Get()};
        Dispatch(*device.context.Get(), { shader.Get(), nullptr, inputs.data(),
            static_cast<UINT>(inputs.size()), output.uav.Get()});
        device.context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(output.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped), "Radial grading math readback");
        const auto pixel = *static_cast<const Float4*>(mapped.pData);
        device.context->Unmap(output.staging.Get(), 0);
        CheckHdrGradeTransferMath(pixel, test, {strength, index, identityStart});
    }
}

struct HdrGradeIntegrationSample
{
    float strength;
    float blacks;
    bool black;
    std::array<float, 4> mix;
    std::size_t pixel{};
};

void ApplyHdrGradeExpectedBlacks(HdrGradeRgb& expected, float blacks)
{
    // Independent double-precision Blacks curve, applied after
    // grade-strength blending and before the neutral other controls.
    const double luminance = HdrGradeLuminance(expected);
    if (blacks != 0 && luminance < 0.02) {
        const double toe = 1.0 - std::max(luminance, 0.0) / 0.02;
        for (double& value : expected) value *= 1.0 + blacks * toe * toe;
    }
}

void CheckHdrGradeIntegrationChannels(const HdrGradeRgb& source,
    const HdrGradeRgb& expected, const HdrGradeRgb& actual,
    const HdrGradeIntegrationSample& sample)
{
    const double tolerance = 3.0e-5 *
        std::max({expected[0], expected[1], expected[2], 1.0e-12});
    for (std::size_t c = 0; c < 3; ++c) {
        // Includes FP32 gamma/decode arithmetic; sampler error has
        // already been isolated by observing the actual samples.
        const double error = std::abs(actual[c] - expected[c]);
        if (error > tolerance) std::cerr << "Radial grading integration"
            << " black_luts=" << sample.black << " weights=" << sample.mix[0] << ',' << sample.mix[1]
            << ',' << sample.mix[2] << ',' << sample.mix[3] << " strength=" << sample.strength
            << " blacks=" << sample.blacks << " pixel=" << sample.pixel << " channel=" << c
            << " expected=" << expected[c] << " actual=" << actual[c]
            << " error=" << error << " tolerance=" << tolerance << '\n';
        FO4HDR_WARP_CHECK(error <= tolerance);
        FO4HDR_WARP_CHECK(std::isfinite(actual[c]) && actual[c] >= 0);
        if (sample.strength == 0 && sample.blacks == 0) FO4HDR_WARP_CHECK(actual[c] == source[c]);
        if (sample.black && sample.strength == 1) FO4HDR_WARP_CHECK(actual[c] == 0);
    }
}

template <class Pixels>
void CheckHdrGradeIntegration(const Pixels& pixels,
    const std::array<HdrGradeRgb, kWidth>& measuredGrade,
    const std::array<HdrGradeRgb, kWidth>& actual,
    const HdrGradeIntegrationSample& sample)
{
    for (std::size_t i = 0; i < kWidth; ++i) {
        const HdrGradeRgb source{Binary16ToFloat(pixels[i][0]),
            Binary16ToFloat(pixels[i][1]), Binary16ToFloat(pixels[i][2])};
        auto expected = DecodeAndTransferNativeGrade(source, measuredGrade[i], sample.strength);
        ApplyHdrGradeExpectedBlacks(expected, sample.blacks);
        auto location = sample;
        location.pixel = i;
        CheckHdrGradeIntegrationChannels(source, expected, actual[i], location);
    }
}

Texture3D CreateBlackHdrGrade(ID3D11Device& gpu)
{
    // A project-created zero 3D texture provides a deliberately black grade.
    auto blackGrade = CreateNativeGrade(gpu, 0);
    {
        D3D11_TEXTURE3D_DESC description{};
        blackGrade.texture->GetDesc(&description);
        description.Usage = D3D11_USAGE_DEFAULT;
        const std::vector<Float4> zero(16 * 16 * 16);
        D3D11_SUBRESOURCE_DATA data{zero.data(), 16 * sizeof(Float4), 16 * 16 * sizeof(Float4)};
        blackGrade = {};
        CheckHresult(gpu.CreateTexture3D(&description, &data,
            &blackGrade.texture), "HDR grading black grade texture");
        CheckHresult(gpu.CreateShaderResourceView(blackGrade.texture.Get(),
            nullptr, &blackGrade.srv), "HDR grading black grade SRV");
    }
    return blackGrade;
}

std::array<ID3D11ShaderResourceView*, 8> HdrGradeIntegrationResources(
    Texture& scene, Texture3D& blackGrade, std::array<Texture3D, 4>& grades, bool black)
{
    std::array<ID3D11ShaderResourceView*, 8> resources{};
    resources[0] = scene.srv.Get();
    for (std::size_t slot = 0; slot < grades.size(); ++slot)
        resources[slot + 3] = black ? blackGrade.srv.Get() : grades[slot].srv.Get();
    return resources;
}
void VerifyHdrGradeTransfer(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sceneSampler)
{
    VerifyHdrGradeTransferMath(device);

    // Observe hardware-filtered codes before applying the independent inverse.
    const auto gradeShader = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kCreativeGradeLut);
    const auto probeShader = CreatePixelShader(*device.device.Get(), fo4hdr::generated::test::kNativeGradeSamplingPs);
    const auto linear = CreateClampLinearSampler(*device.device.Get());
    std::array<Texture3D, 4> grades;
    for (unsigned slot = 0; slot < 4; ++slot)
        grades[slot] = CreateNativeGrade(*device.device.Get(), slot);
    auto blackGrade = CreateBlackHdrGrade(*device.device.Get());
    const auto pixels = MakeFp16Pixels(std::array<Float4, kWidth>{{
        {0,0,0,1}, {0.00006103515625F,0.0001220703125F,0.000244140625F,1},
        {0.001F,0.001F,0.001F,1}, {0.18F,0.18F,0.18F,1},
        {4,1,0.25F,1}, {1,4,0.25F,1}, {0.25F,1,16,1}, {64,16,4,1}}});
    D3D11_SUBRESOURCE_DATA sceneData{pixels.data(), sizeof(pixels), 0};
    auto scene = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE, &sceneData);
    auto sampled = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto guide = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto transferred = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    const auto read = [&](Texture& texture) {
        device.context->CopyResource(texture.staging.Get(), texture.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(texture.staging.Get(), 0, D3D11_MAP_READ,
            0, &mapped), "HDR grading integration readback");
        const auto* values = static_cast<const Float4*>(mapped.pData);
        std::array<HdrGradeRgb, kWidth> result{};
        for (std::size_t i = 0; i < kWidth; ++i)
            result[i] = {values[i].red, values[i].green, values[i].blue};
        device.context->Unmap(texture.staging.Get(), 0);
        return result;
    };
    constexpr std::array<std::array<float, 4>, 8> mixes{{
        {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1},
        {0.25F,0.25F,0.25F,0.25F}, {0,0.75F,0.25F,0},
        {0,0.25F,0.75F,0}, {0.1F,0.2F,0.3F,0.4F}}};
    std::array<ID3D11SamplerState*, 8> samplers{sceneSampler, nullptr, nullptr,
        linear.Get(), linear.Get(), linear.Get(), linear.Get(), linear.Get()};
    for (const bool black : {false, true})
    for (const auto& mix : mixes) {
        auto resources = HdrGradeIntegrationResources(scene, blackGrade, grades, black);
        LutConstants weights{};
        weights.values[1] = {mix[0], mix[1], mix[2], mix[3]};
        auto lutConstants = CreateConstantBuffer(*device.device.Get(), weights);
        std::array<ID3D11RenderTargetView*, 2> targets{sampled.rtv.Get(), guide.rtv.Get()};
        ToneMapConstants settings{0,1000,203,203,1,203,0,1};
        auto constants = CreateConstantBuffer(*device.device.Get(), settings);
        DrawFullscreen(*device.context.Get(), { vertexShader, probeShader.Get(),
            lutConstants.Get(), constants.Get(), resources.data(), 8,
            samplers.data(), 8, targets.data(), 2});
        const auto measuredGrade = read(sampled);

        for (const float strength : {0.0F, 0.5F, 1.0F})
        for (const float blacks : {-0.75F, 0.0F, 0.75F}) {
            settings.vanillaGradeStrength = strength;
            settings.blacksStrength = blacks;
            constants = CreateConstantBuffer(*device.device.Get(), settings);
            ID3D11RenderTargetView* target = transferred.rtv.Get();
            DrawFullscreen(*device.context.Get(), { vertexShader, gradeShader.Get(),
                lutConstants.Get(), constants.Get(), resources.data(), 8,
                samplers.data(), 8, &target, 1});
            const auto actual = read(transferred);
            CheckHdrGradeIntegration(pixels, measuredGrade, actual,
                {strength, blacks, black, mix});
        }
    }
}

}  // namespace fo4hdr::tests::gpu
