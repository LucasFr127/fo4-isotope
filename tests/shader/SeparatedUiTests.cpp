// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

namespace fo4hdr::tests::gpu {

struct PublicPresentationInputs
{
    Texture& scene;
    Texture& layer;
    Texture& output;
    const PresentationShaders* shader;
};

double DecodePublicPresentationChannel(double code, UINT encoding)
{
    if (encoding == 2) return std::copysign(std::pow(std::abs(code), 2.2), code);
    if (encoding == 0) return code;
    const double magnitude = std::abs(code);
    const double linear = magnitude <= 0.04045 ? magnitude / 12.92 :
        std::pow((magnitude + 0.055) / 1.055, 2.4);
    return std::copysign(linear, code);
}

double PublicPresentationSourceChannel(double channel, const CompositionConstants& constants)
{
    if (constants.mode == 4 || constants.mode == 5)
        channel = std::isnan(channel) ? 0.0 : std::clamp(channel, -65504.0, 65504.0);
    return DecodePublicPresentationChannel(channel, constants.sharedUiEncoded);
}

bool PublicPresentationNeedsPrimaryExpansion(const CompositionConstants& constants)
{
    return constants.sharedUiEncoded != 0 || constants.mode == 1 || constants.mode == 5;
}

fo4hdr::LinearRgb ExpectedPublicPresentationSource(const Float4& pixel,
    const CompositionConstants& constants)
{
    std::array<double, 3> channels{pixel.red, pixel.green, pixel.blue};
    for (auto& channel : channels) channel = PublicPresentationSourceChannel(channel, constants);
    fo4hdr::LinearRgb light{channels[0], channels[1], channels[2]};
    if (PublicPresentationNeedsPrimaryExpansion(constants))
        light = fo4hdr::ConvertBt709ToBt2020(light);
    const double maximum = std::max({light.red, light.green, light.blue});
    const double peak = constants.armorPeakUiRelative;
    if (constants.sharedUiEncoded == 2 && peak > 0.0) {
        const double scale = peak / std::max(peak, maximum);
        light = {light.red * scale, light.green * scale, light.blue * scale};
    }
    return light;
}

fo4hdr::LinearRgb ExpectedPublicPresentationPixel(const Float4& pixel,
    const CompositionConstants& constants)
{
    auto light = ExpectedPublicPresentationSource(pixel, constants);
    if (constants.settingsOverlayPresent != 0) {
        const auto layer = fo4hdr::ConvertBt709ToBt2020({
            DecodePublicPresentationChannel(0.8, 1) * 0.25,
            DecodePublicPresentationChannel(0.4, 1) * 0.25,
            DecodePublicPresentationChannel(0.2, 1) * 0.25});
        light = {light.red * 0.75 + layer.red, light.green * 0.75 + layer.green,
            light.blue * 0.75 + layer.blue};
    }
    if (constants.mode == 4 || constants.mode == 5) {
        light = fo4hdr::ConvertBt2020ToBt709ForComparison(light);
        return {std::pow(std::clamp(light.red, 0.0, 1.0), 1.0 / 2.2),
            std::pow(std::clamp(light.green, 0.0, 1.0), 1.0 / 2.2),
            std::pow(std::clamp(light.blue, 0.0, 1.0), 1.0 / 2.2)};
    }
    const double white = constants.uiPaperWhiteNits;
    return fo4hdr::EncodeSt2084Pq({light.red * white, light.green * white, light.blue * white});
}

struct PublicPresentationReadback
{
    Texture& output;
    const std::array<Float4, 37 * 19>& pixels;
    CompositionConstants constants;
};

void CheckPublicPresentationPixels(Device& device, const PublicPresentationReadback& test)
{
    device.context->CopyResource(test.output.staging.Get(), test.output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(test.output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read public source output");
    for (UINT y = 0; y < 19; ++y) {
        const auto* row = reinterpret_cast<const UINT*>(
            static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < 37; ++x) {
            const auto expected = ExpectedPublicPresentationPixel(test.pixels[y * 37 + x], test.constants);
            const std::array values{expected.red, expected.green, expected.blue};
            for (UINT channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>((row[x] >> (channel * 10)) & 1023),
                    fo4hdr::QuantizeR10Unorm(values[channel])) <= 1);
            FO4HDR_WARP_CHECK(row[x] >> 30 == 3);
        }
    }
    device.context->Unmap(test.output.staging.Get(), 0);
}

struct PublicSourceMapping
{
    UINT mode, encoding;
};

void VerifyPublicPresentationMapping(Device& device, const PublicPresentationInputs& test,
    PublicSourceMapping source, const std::array<Float4, 37 * 19>& pixels)
{
    constexpr UINT width = 37, height = 19;
    for (const float white : {120.0F, 280.0F}) {
        for (const UINT hasSettings : {0U, 1U}) {
            const CompositionConstants mapping{width, height, source.mode, white, white, 1000,
                0, 0, hasSettings, source.encoding, 0, source.encoding == 2 ? 2.5F : 0.0F};
            const auto constants = CreateConstantBuffer(*device.device.Get(), mapping);
            DrawPresentation(device, {test.shader, constants.Get(),
                {test.scene.srv.Get(), nullptr, nullptr, test.layer.srv.Get()},
                test.output.rtv.Get(), width, height});
            CheckPublicPresentationPixels(device, {test.output, pixels, mapping});
        }
    }
}

void VerifyPublicPresentationPixels(Device& device, const PublicPresentationInputs& test)
{
    constexpr UINT width = 37, height = 19;
    constexpr std::array samples{
        Float4{0.0F, 0.0F, 0.0F, 1.0F}, Float4{0.18F, 0.18F, 0.18F, 1.0F},
        Float4{0.75F, 0.25F, 0.125F, 1.0F}, Float4{-0.25F, 0.5F, 1.5F, 1.0F},
        Float4{3.0F, 2.0F, 1.0F, 1.0F}, Float4{-1.0F, -1.0F, -1.0F, 1.0F}};
    std::array<Float4, width * height> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = samples[index % samples.size()];
    device.context->UpdateSubresource(test.scene.texture.Get(), 0, nullptr, pixels.data(), width * sizeof(Float4), 0);
    std::array<Float4, width * height> overlay{};
    overlay.fill({0.2F, 0.1F, 0.05F, 0.25F});
    device.context->UpdateSubresource(test.layer.texture.Get(), 0, nullptr, overlay.data(), width * sizeof(Float4), 0);
    constexpr std::array mappings{PublicSourceMapping{1, 0}, PublicSourceMapping{3, 0},
        PublicSourceMapping{3, 1}, PublicSourceMapping{3, 2}, PublicSourceMapping{4, 0},
        PublicSourceMapping{4, 1}, PublicSourceMapping{4, 2}, PublicSourceMapping{5, 0}};
    for (const auto mapping : mappings) VerifyPublicPresentationMapping(device, test, mapping, pixels);
}

void VerifyPublicSdrContainment(Device& device, const PublicPresentationInputs& test)
{
    constexpr UINT width = 37, height = 19;
    constexpr std::array extremes{-1.0F, 0.0F, 1.0e-7F, 0.18F, 1.0F, 65504.0F,
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()};
    std::array<Float4, width * height> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const float value = extremes[index % extremes.size()];
        pixels[index] = {value, value, value, 1};
    }
    device.context->UpdateSubresource(test.scene.texture.Get(), 0, nullptr, pixels.data(), width * sizeof(Float4), 0);
    for (const UINT mode : {4U, 5U}) {
        for (const UINT encoding : {0U, 1U, 2U}) {
            if (mode == 5 && encoding != 0) continue;
            const CompositionConstants mapping{width, height, mode, 203, 203, 1000, 0, 0, 0, encoding};
            const auto constants = CreateConstantBuffer(*device.device.Get(), mapping);
            DrawPresentation(device, {test.shader, constants.Get(), {test.scene.srv.Get(), nullptr, nullptr, nullptr},
                test.output.rtv.Get(), width, height});
            CheckPublicPresentationPixels(device, {test.output, pixels, mapping});
        }
    }
}

void VerifyPublicPresentationOutput(Device& device)
{
    constexpr UINT width = 37, height = 19;
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, width, height);
    auto output = GetPresentationBuffer(device, *chain.Get());
    auto scene = CreateSizedTexture(*device.device.Get(), {width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE});
    auto layer = CreateSizedTexture(*device.device.Get(), {width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE});
    const auto shader = CreatePresentationShaders(*device.device.Get());
    const PublicPresentationInputs inputs{scene, layer, output, &shader};
    VerifyPublicPresentationPixels(device, inputs);
    VerifyPublicSdrContainment(device, inputs);
}

bool CheckUiRelativeTemporalScene(fo4hdr::LinearRgb sceneNits)
{
    const std::array channels{sceneNits.red, sceneNits.green, sceneNits.blue};
    bool preservedAboveOne{};
    for (const float white : {100.0F, 203.0F, 300.0F}) {
        for (const double channel : channels) {
            const auto stored = Binary16ToFloat(
                FloatToBinary16(static_cast<float>(channel / white)));
            FO4HDR_WARP_CHECK(std::isfinite(stored) && stored >= 0.0F);
            preservedAboveOne = preservedAboveOne || stored > 1.0F;
            // FP16 UI-relative storage may shift fixed-203-nit R8 history by one code.
            const double compensated = static_cast<double>(stored) * white / kReferenceWhiteNits;
            const double fixedReference = channel / kReferenceWhiteNits;
            const auto actualHistory = TemporalHistoryCode(
                fo4hdr::EncodeTemporalIntermediate(compensated));
            const auto expectedHistory = TemporalHistoryCode(
                fo4hdr::EncodeTemporalIntermediate(fixedReference));
            FO4HDR_WARP_CHECK(std::max(actualHistory, expectedHistory) -
                std::min(actualHistory, expectedHistory) <= 1U);
            const double decoded = fo4hdr::DecodeTemporalIntermediate(
                fo4hdr::EncodeTemporalIntermediate(compensated)) * kReferenceWhiteNits / white;
            FO4HDR_WARP_CHECK(std::abs(decoded - static_cast<double>(stored)) <=
                std::max(1.0e-9, static_cast<double>(stored) * 1.0e-9));
        }
    }
    return preservedAboveOne;
}

void VerifyUiRelativeTemporalTransport(TestedToneMapper toneMapper)
{
    constexpr std::array<Float4, kWidth> sourceScene{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {8.0F, 4.0F, 2.0F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {2.0F, 0.5F, 0.125F, 1.0F},
        {0.125F, 2.0F, 0.5F, 1.0F},
        {0.5F, 0.125F, 2.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F},
        {8.0F, 8.0F, 8.0F, 1.0F},
    }};
    bool preservedAboveOne{};
    for (const float peak : {400.0F, 1000.0F, 2000.0F}) {
        for (const auto& source : sourceScene) {
            const auto sceneNits = MapProductionOperator(
                toneMapper, {source.red, source.green, source.blue}, peak);
            preservedAboveOne = CheckUiRelativeTemporalScene(sceneNits) || preservedAboveOne;
        }
    }
    FO4HDR_WARP_CHECK(preservedAboveOne);
}

}  // namespace fo4hdr::tests::gpu
