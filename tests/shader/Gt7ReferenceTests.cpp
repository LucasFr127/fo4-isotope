// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace fo4hdr::tests::gpu {

namespace Gt7ReferenceData {
    constexpr std::array<Float4, kWidth> official{{
        {0.5F, 1.23F, 0.75F, 1.0F},
        {12.3F, 34.3F, 56.9F, 1.0F},
        {1504.7F, 64.51F, 0.5F, 1.0F},
        {0.0F, 0.0F, 0.0F, 1.0F},
        {8.0F, 0.0F, 0.0F, 1.0F},
        {0.0F, 8.0F, 16.0F, 1.0F},
        {0.0F, 0.60791015625F, 54.21875F, 1.0F},
        {0.0F, 2.6875F, 28.734375F, 1.0F}
    }};
    constexpr std::array<Float4, kWidth> neutral{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {0.001F, 0.001F, 0.001F, 1.0F},
        {0.01F, 0.01F, 0.01F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {2.0F, 2.0F, 2.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F},
        {16.0F, 16.0F, 16.0F, 1.0F}
    }};
    constexpr std::array<Float4, kWidth> natural{{
        {8.0F, 8.0F, 8.0F, 1.0F},
        {3.84375F, 3.8125F, 3.5625F, 1.0F},
        {3.46875F, 3.53125F, 3.25F, 1.0F},
        {2.03125F, 0.6875F, 0.53125F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {2.0F, 2.0F, 2.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F}
    }};
    constexpr std::array<Float4, kWidth> chromatic{{
        {8.0F, 0.0F, 0.0F, 1.0F},
        {0.0F, 8.0F, 0.0F, 1.0F},
        {0.0F, 0.0F, 8.0F, 1.0F},
        {8.0F, 8.0F, 0.0F, 1.0F},
        {0.0F, 8.0F, 8.0F, 1.0F},
        {8.0F, 0.0F, 8.0F, 1.0F},
        {2.03125F, 0.6875F, 0.53125F, 1.0F},
        {0.0F, 0.0F, 0.5F, 1.0F}
    }};
}
std::array<std::array<Float4, kWidth>, 4> BuildGt7ReferenceCorpora()
{
    using namespace Gt7ReferenceData;
    std::array<std::array<Float4, kWidth>, 4> corpora{};
    corpora[0] = official;
    const std::array production{neutral, natural, chromatic};
    for (std::size_t corpus = 0; corpus < production.size(); ++corpus)
    {
        for (std::size_t index = 0; index < kWidth; ++index)
        {
            const auto& pixel = production[corpus][index];
            const auto decodeScene = [](float value) noexcept {
                return Binary16ToFloat(FloatToBinary16(value));
            };
            const auto rec2020 = fo4hdr::ConvertBt709ToBt2020({
                decodeScene(pixel.red),
                decodeScene(pixel.green),
                decodeScene(pixel.blue)});
            constexpr float sceneScale = 203.0F / 100.0F;
            corpora[corpus + 1][index] = {
                static_cast<float>(rec2020.red) * sceneScale,
                static_cast<float>(rec2020.green) * sceneScale,
                static_cast<float>(rec2020.blue) * sceneScale,
                1.0F
            };
        }
    }
    return corpora;
}

struct Gt7ReferenceCase
{
    std::size_t corpus;
    float peak;
    std::size_t index;
    bool CrossTalk(std::size_t channel, double expected) const
    {
        return corpus == 0 && index >= 6 && channel == 0 && expected > 1.0e-5;
    }
};
struct Gt7ReferenceChannel
{
    double expected;
    float actual;
    unsigned distance;
    double vectorScaleBound;
};
bool AcceptGt7ReferenceChannel(Gt7ReferenceChannel sample, bool crossTalk)
{
    const auto& [expected, actual, distance, vectorScaleBound] = sample;
    if (crossTalk) return expected > 0.0 && actual > 0.0F &&
        std::abs(static_cast<double>(actual) - expected) <= vectorScaleBound;
    // Real hardware may fuse cancellation producing an exact-zero channel;
    // retain the narrow absolute 0.01-nit envelope for those official samples.
    // The CPU source port remains independently held to two float ULPs over
    // the broad corpus.
    return distance <= 2U || (expected <= 1.0e-5 &&
        std::abs(static_cast<double>(actual) - expected) <= 1.0e-4);
}
void CheckGt7ReferencePixel(const std::array<std::uint16_t, 4>& values,
    const Float4& pixel, Gt7ReferenceCase test)
{
    const auto& [corpus, peak, index] = test;
    const fo4hdr::LinearRgb input{
        pixel.red, pixel.green, pixel.blue};
    const auto expected =
        fo4hdr::MapGt7ReferenceRec2020(input, peak);
    const std::array channels{
        expected.red, expected.green, expected.blue};
    for (std::size_t channel = 0; channel < channels.size();
         ++channel)
    {
        const auto actual =
            Binary16ToFloat(values[channel]);
        const auto expectedHalf =
            FloatToBinary16(static_cast<float>(channels[channel]));
        const auto distance =
            CodeDistance(values[channel], expectedHalf);
        // These additional zero-red vectors cannot originate in
        // the strictly positive BT.709->BT.2020 scene adapter.
        // They guard legitimate inverse-ICtCp cross-talk only;
        // its cancellation-conditioned red error is bounded by
        // the same 2.5-bin envelope at the native vector scale.
        const bool crossTalkRegression = test.CrossTalk(channel, channels[channel]);
        const auto largestNative = static_cast<float>(std::max({
            channels[0], channels[1], channels[2]}));
        const auto vectorScaleBound = 2.5 *
            Binary16LargestAdjacentStep(largestNative);
        const auto acceptable = AcceptGt7ReferenceChannel(
            {channels[channel], actual, distance, vectorScaleBound}, crossTalkRegression);
        if (!acceptable)
        {
            std::cerr << "GT7 official Rec.2020 parity: corpus="
                      << corpus << " peak=" << peak
                      << " sample=" << index
                      << " channel=" << channel
                      << " actual=" << actual
                      << " expected=" << channels[channel]
                      << " half_ulp=" << distance
                      << " vector_scale_bound="
                      << vectorScaleBound << '\n';
        }
        FO4HDR_WARP_CHECK(std::isfinite(actual));
        FO4HDR_WARP_CHECK(acceptable);
    }
}

void VerifyGt7OfficialReference(Device& device)
{
    const auto corpora = BuildGt7ReferenceCorpora();
    std::array<Float4, kWidth> transparent{};
    const auto uiPixels = MakeFp16Pixels(transparent);
    D3D11_SUBRESOURCE_DATA uiData{
        uiPixels.data(), static_cast<UINT>(sizeof(uiPixels)), 0};
    auto ui = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE, &uiData);
    auto composition = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    const auto shader = CreateShader(
        *device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);

    for (std::size_t corpus = 0; corpus < corpora.size(); ++corpus)
    {
        D3D11_SUBRESOURCE_DATA sceneData{
            corpora[corpus].data(),
            static_cast<UINT>(sizeof(corpora[corpus])),
            0};
        // Preserve the exact float adapter input; inserting an FP16 source
        // quantization here would test a different nonlinear GT7 color.
        auto scene = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &sceneData);
        for (const auto peak : {400.0F, 1000.0F, 2000.0F,
                 4000.0F, 10000.0F})
        {
            const auto constants = CreateConstantBuffer(
                *device.device.Get(), CompositionConstants{
                    kWidth, kHeight, 8U, 203.0F, 203.0F, peak, 0U, 0U});
            std::array<ID3D11ShaderResourceView*, 4> inputs{
                scene.srv.Get(), nullptr, nullptr, ui.srv.Get()};
            Dispatch(*device.context.Get(), { shader.Get(), constants.Get(),
                inputs.data(), static_cast<UINT>(inputs.size()),
                composition.uav.Get()});
            device.context->CopyResource(
                composition.staging.Get(), composition.texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CheckHresult(device.context->Map(composition.staging.Get(), 0,
                D3D11_MAP_READ, 0, &mapped),
                "Map official GT7 Rec.2020 framebuffer reference");
            const auto* values = static_cast<
                const std::array<std::uint16_t, 4>*>(mapped.pData);
            for (std::size_t index = 0; index < kWidth; ++index)
            {
                CheckGt7ReferencePixel(values[index], corpora[corpus][index], {corpus, peak, index});
            }
            device.context->Unmap(composition.staging.Get(), 0);
        }
    }
}

}  // namespace fo4hdr::tests::gpu
