// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ImageControlReference.hpp"
#include "reference/ShaderReference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace fo4hdr::tests::gpu {
namespace {

constexpr NativeGradeRgb kFilterLuma{0.212500006, 0.715399981, 0.0720999986};
constexpr float kMeasuredExposure = 0.5F;
// Every value is exact in FP16, so the oracle sees the stored inputs.
constexpr std::array<Float4, kWidth> kSdrScene{{
    {0.0F, 0.0F, 0.0F, 1.0F},
    {0.1875F, 0.1875F, 0.1875F, 1.0F},
    {0.5F, 0.5F, 0.5F, 0.5F},
    {1.0F, 1.0F, 1.0F, 1.0F},
    {2.5F, 2.5F, 2.5F, 1.0F},
    {6.0F, 6.0F, 6.0F, 1.0F},
    {3.0F, 0.75F, 0.25F, 1.0F},
    {0.25F, 1.5F, 0.0625F, 0.25F},
}};
constexpr Float4 kSdrBloom{0.0625F, 0.125F, 0.25F, 1.0F};

// Values exercise every vanilla filter term: saturation, tint, brightness,
// contrast around the measured exposure, and the B variants' color lerp.
ImageSpaceConstantsWithFinalAffine SdrImageSpace(float toeNumerator)
{
    ImageSpaceConstantsWithFinalAffine constants{};
    constants.values[1] = {4.0F, 0.25F, 0.5F, toeNumerator};
    constants.values[2] = {1.2F, 0.0F, 0.9F, 1.1F};
    constants.values[3] = {1.0F, 0.9F, 0.8F, 0.1F};
    constants.values[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    constants.values[5] = {0.2F, 0.3F, 0.4F, 0.1F};
    return constants;
}

// Oracle for the pinned RenoDX vanilla tone map after exposure: the
// Uncharted 2 curve with the live toe numerator, then the ImageSpace filter.
NativeGradeRgb VanillaToneMap(const NativeGradeRgb& exposed,
    const ImageSpaceConstantsWithFinalAffine& cb2, bool finalAffine)
{
    const auto& values = cb2.values;
    const double toe = values[1].alpha;
    const double white = (toe * 0.2 + 19.3759995) * 0.0408563502 - toe * 3.333333;
    NativeGradeRgb curved{};
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const double x = exposed[channel];
        curved[channel] = ((2.0 * x * (0.3 * x + 0.05) + 0.2 * toe) /
            (2.0 * x * (0.3 * x + 0.5) + 0.06) - toe * 3.333333) / white;
    }
    const double luminance = curved[0] * kFilterLuma[0] + curved[1] * kFilterLuma[1] +
        curved[2] * kFilterLuma[2];
    const std::array<double, 3> tint{values[3].red, values[3].green, values[3].blue};
    const std::array<double, 3> fade{values[5].red, values[5].green, values[5].blue};
    NativeGradeRgb result{};
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        double value = values[2].red * (curved[channel] - luminance) + luminance;
        value += values[3].alpha * (luminance * tint[channel] - value);
        value = values[2].blue * (values[2].alpha * value - kMeasuredExposure) + kMeasuredExposure;
        if (finalAffine) value += values[5].alpha * (fade[channel] - value);
        result[channel] = value;
    }
    return result;
}

std::array<Float4, kWidth> ReadPixels(Device& device, Texture& output)
{
    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map SDR pixel-shader output");
    std::array<Float4, kWidth> pixels{};
    const auto* values = static_cast<const Float4*>(mapped.pData);
    std::copy(values, values + kWidth, pixels.begin());
    device.context->Unmap(output.staging.Get(), 0);
    return pixels;
}

void CheckPixel(const Float4& actual, const NativeGradeRgb& expected,
    std::string_view label, std::size_t sample)
{
    const std::array<double, 3> values{actual.red, actual.green, actual.blue};
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const bool acceptable = std::isfinite(values[channel]) &&
            std::abs(values[channel] - expected[channel]) <= 1.0e-4;
        if (!acceptable)
            std::cerr << label << ": sample=" << sample << " channel=" << channel
                      << " actual=" << values[channel] << " expected=" << expected[channel] << '\n';
        FO4HDR_WARP_CHECK(acceptable);
    }
}

[[nodiscard]] Texture CreatePixelTexture(Device& device, const std::array<Float4, kWidth>& pixels)
{
    const auto fp16 = MakeFp16Pixels(pixels);
    D3D11_SUBRESOURCE_DATA data{fp16.data(), static_cast<UINT>(sizeof(fp16)), 0};
    return CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE, &data);
}

struct SdrToneMapFixture
{
    Device& device;
    ID3D11VertexShader* vertexShader;
    ID3D11SamplerState* sampler;
    Texture scene;
    Texture bloom;
    Texture exposure;
    Texture depth;
    Texture bypassDepth;
    Texture output;
    std::array<ComPtr<ID3D11PixelShader>, 4> shaders;

    struct Case
    {
        std::string_view label;
        ToneMapConstants settings;
        float toeNumerator;
        bool bypass;
    };

    void Run(const Case& test)
    {
        constexpr std::array names{"ToneMapASdr", "ToneMapBSdr", "ToneMapNewASdr", "ToneMapNewBSdr"};
        const auto cb2 = SdrImageSpace(test.toeNumerator);
        const auto imageSpace = CreateConstantBuffer(*device.device.Get(), cb2);
        const auto settings = CreateConstantBuffer(*device.device.Get(), test.settings);
        auto* depthView = test.bypass ? bypassDepth.srv.Get() : depth.srv.Get();
        for (std::size_t variant = 0; variant < shaders.size(); ++variant)
        {
            const bool newVariant = variant >= 2;
            const bool finalAffine = (variant & 1U) != 0U;
            std::array<ID3D11ShaderResourceView*, 4> resources{
                newVariant ? scene.srv.Get() : bloom.srv.Get(),
                newVariant ? bloom.srv.Get() : scene.srv.Get(),
                exposure.srv.Get(), depthView};
            std::array<ID3D11SamplerState*, 4> samplers{sampler, sampler, sampler, sampler};
            ID3D11RenderTargetView* targets[]{output.rtv.Get()};
            DrawFullscreen(*device.context.Get(), {vertexShader, shaders[variant].Get(),
                imageSpace.Get(), settings.Get(), resources.data(), 4U,
                samplers.data(), 4U, targets, 1});
            const auto pixels = ReadPixels(device, output);
            for (std::size_t sample = 0; sample < kWidth; ++sample)
                CheckPixel(pixels[sample], Expected(test, cb2, finalAffine, kSdrScene[sample]),
                    std::string(names[variant]) + " " + std::string(test.label), sample);
        }
    }

    [[nodiscard]] static NativeGradeRgb Expected(const Case& test,
        const ImageSpaceConstantsWithFinalAffine& cb2, bool finalAffine, const Float4& scene)
    {
        if (test.bypass) return {scene.red, scene.green, scene.blue};
        const double exposure = std::clamp(0.5 / (0.001 + kMeasuredExposure), 0.25, 4.0);
        const double gain = exposure * test.settings.sceneExposureMultiplier;
        const double bloomScale = test.settings.bloomStrength;
        NativeGradeRgb exposed{(scene.red + kSdrBloom.red * bloomScale) * gain,
            (scene.green + kSdrBloom.green * bloomScale) * gain,
            (scene.blue + kSdrBloom.blue * bloomScale) * gain};
        exposed = ApplyImageControlsOracle(exposed, test.settings.shadowsEv,
            test.settings.contrastSlope, test.settings.blacksStrength);
        return VanillaToneMap(exposed, cb2, finalAffine);
    }
};

[[nodiscard]] ToneMapConstants NeutralSdrConstants() noexcept
{
    return {0U, 1000.0F, kReferenceWhiteNits, kReferenceWhiteNits,
        1.0F, kReferenceWhiteNits, 0.0F, 0.0F};
}

void VerifySdrToneMapPixelShaders(Device& device, ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler)
{
    auto& d3d = *device.device.Get();
    SdrToneMapFixture fixture{device, vertexShader, sampler,
        CreatePixelTexture(device, kSdrScene),
        CreatePixelTexture(device, UniformPixels(kSdrBloom)),
        CreatePixelTexture(device, UniformPixels({kMeasuredExposure, 0.0F, 0.0F, 1.0F})),
        CreatePixelTexture(device, UniformPixels({})),
        // Fallout 4's raw alpha tag 4/255 bypasses exposure, bloom, curve and filter.
        CreatePixelTexture(device, UniformPixels({0.0F, 0.0F, 0.0F, 4.0F / 255.0F})),
        CreateTexture(d3d, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET),
        {CreatePixelShader(d3d, fo4hdr::generated::kToneMapASdr),
            CreatePixelShader(d3d, fo4hdr::generated::kToneMapBSdr),
            CreatePixelShader(d3d, fo4hdr::generated::kToneMapNewASdr),
            CreatePixelShader(d3d, fo4hdr::generated::kToneMapNewBSdr)}};

    // Neutral controls reproduce the vanilla curve and filter for both toe values.
    fixture.Run({"neutral", NeutralSdrConstants(), 0.02F, false});
    fixture.Run({"neutral toe", NeutralSdrConstants(), 0.035F, false});
    fixture.Run({"bypass", NeutralSdrConstants(), 0.02F, true});
    // Exposure and Bloom Strength act on the scene before the curve.
    auto exposed = NeutralSdrConstants();
    exposed.sceneExposureMultiplier = 2.0F;
    exposed.bloomStrength = 0.5F;
    fixture.Run({"exposure and bloom", exposed, 0.02F, false});
    // Image controls act in scene-linear light, also before the curve.
    auto controlled = NeutralSdrConstants();
    controlled.shadowsEv = 0.5F;
    controlled.contrastSlope = 1.25F;
    fixture.Run({"image controls", controlled, 0.02F, false});
}

[[nodiscard]] Texture3D CreateUniformLut(ID3D11Device& device, Float4 code)
{
    constexpr UINT size = 16;
    std::array<Float4, size * size * size> texels{};
    texels.fill(code);
    D3D11_TEXTURE3D_DESC description{};
    description.Width = size;
    description.Height = size;
    description.Depth = size;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{texels.data(), static_cast<UINT>(size * sizeof(Float4)),
        static_cast<UINT>(size * size * sizeof(Float4))};
    Texture3D result;
    CheckHresult(device.CreateTexture3D(&description, &data, &result.texture),
        "ID3D11Device::CreateTexture3D(uniform LUT)");
    CheckHresult(device.CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv),
        "ID3D11Device::CreateShaderResourceView(uniform LUT)");
    return result;
}

[[nodiscard]] NativeGradeRgb EncodeGamma22(const NativeGradeRgb& light)
{
    return {std::pow(light[0], 1.0 / 2.2), std::pow(light[1], 1.0 / 2.2),
        std::pow(light[2], 1.0 / 2.2)};
}

void VerifySdrGradeLutPixelShader(Device& device, ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sceneSampler)
{
    auto& d3d = *device.device.Get();
    constexpr std::array<Float4, kWidth> displayPixels{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {0.0625F, 0.0625F, 0.0625F, 1.0F},
        {0.25F, 0.25F, 0.25F, 0.5F},
        {0.5F, 0.5F, 0.5F, 1.0F},
        {0.75F, 0.75F, 0.75F, 1.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {1.5F, 1.5F, 1.5F, 1.0F},
        {0.75F, 0.25F, 0.0625F, 0.25F},
    }};
    const auto scene = CreatePixelTexture(device, displayPixels);
    constexpr Float4 firstCode{0.2F, 0.4F, 0.8F, 1.0F};
    constexpr Float4 secondCode{0.6F, 0.3F, 0.1F, 1.0F};
    const auto first = CreateUniformLut(d3d, firstCode);
    const auto second = CreateUniformLut(d3d, secondCode);
    const auto identity = CreateIdentityLut(d3d);
    const auto lutSampler = CreateClampLinearSampler(d3d);
    auto output = CreateTexture(d3d, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    const auto shader = CreatePixelShader(d3d, fo4hdr::generated::kCreativeGradeLutSdr);
    LutConstants weights{};
    weights.values[1] = {0.25F, 0.75F, 0.0F, 0.0F};
    const auto cb2 = CreateConstantBuffer(d3d, weights);
    const NativeGradeRgb codes{0.25 * firstCode.red + 0.75 * secondCode.red,
        0.25 * firstCode.green + 0.75 * secondCode.green,
        0.25 * firstCode.blue + 0.75 * secondCode.blue};

    struct Case { std::string_view label; float strength; float luminanceRemoval; float colorRemoval; };
    for (const Case test : {Case{"default", 1.0F, 0.0F, 0.0F}, Case{"strength 0", 0.0F, 0.0F, 0.0F},
             Case{"strength half", 0.5F, 0.0F, 0.0F}, Case{"full removal", 1.0F, 1.0F, 1.0F},
             Case{"luminance removal", 1.0F, 0.5F, 0.0F}, Case{"color removal", 1.0F, 0.0F, 0.75F}})
    {
        auto settings = NeutralSdrConstants();
        settings.vanillaGradeStrength = test.strength;
        settings.selectiveColorPlaneA.alpha = test.luminanceRemoval;
        settings.selectiveColorPlaneB.alpha = test.colorRemoval;
        const auto settingsBuffer = CreateConstantBuffer(d3d, settings);
        std::array<ID3D11ShaderResourceView*, 7> resources{scene.srv.Get(), nullptr, nullptr,
            first.srv.Get(), second.srv.Get(), identity.srv.Get(), identity.srv.Get()};
        std::array<ID3D11SamplerState*, 7> samplers{sceneSampler, nullptr, nullptr,
            lutSampler.Get(), lutSampler.Get(), lutSampler.Get(), lutSampler.Get()};
        ID3D11RenderTargetView* targets[]{output.rtv.Get()};
        DrawFullscreen(*device.context.Get(), {vertexShader, shader.Get(), cb2.Get(),
            settingsBuffer.Get(), resources.data(), 7U, samplers.data(), 7U, targets, 1});
        const auto pixels = ReadPixels(device, output);
        for (std::size_t sample = 0; sample < kWidth; ++sample)
        {
            const auto& input = displayPixels[sample];
            const NativeGradeRgb display{std::min(input.red, 1.0F), std::min(input.green, 1.0F),
                std::min(input.blue, 1.0F)};
            NativeGradeRgb expected = codes;
            const bool blended = test.strength < 1.0F || test.luminanceRemoval > 0.0F ||
                test.colorRemoval > 0.0F;
            if (blended)
            {
                NativeGradeRgb graded{};
                for (std::size_t channel = 0; channel < 3; ++channel)
                    graded[channel] = display[channel] + test.strength *
                        (std::pow(codes[channel], 2.2) - display[channel]);
                expected = EncodeGamma22(ApplyVanillaGradeRemovalOracle(
                    display, graded, test.luminanceRemoval, test.colorRemoval));
            }
            CheckPixel(pixels[sample], expected, test.label, sample);
            FO4HDR_WARP_CHECK(pixels[sample].alpha == input.alpha);
        }
    }
}

}  // namespace

void VerifySdrPixelShaders(Device& device, ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler)
{
    VerifySdrToneMapPixelShaders(device, vertexShader, sampler);
    VerifySdrGradeLutPixelShader(device, vertexShader, sampler);
}

}  // namespace fo4hdr::tests::gpu
