// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "Fallout4HDR/tonemap/Agx.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace fo4hdr::tests::gpu {

struct Gt7Nits
{
    std::array<fo4hdr::LinearRgb, kWidth> actual{};
    std::array<fo4hdr::LinearRgb, kWidth> reference{};
};
struct Gt7OperatorUi
{
    Device& device;
    static constexpr auto compositionMode = 7U;
    static constexpr auto algorithmName = "GT7";
    std::array<bool, 2> gt7PairwiseDifferences{};
    static constexpr std::array<Float4, kWidth> neutral{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {0.001F, 0.001F, 0.001F, 1.0F},
        {0.01F, 0.01F, 0.01F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {2.0F, 2.0F, 2.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F},
        {16.0F, 16.0F, 16.0F, 1.0F}
    }};
    static constexpr std::array<Float4, kWidth> natural{{
        {8.0F, 8.0F, 8.0F, 1.0F},
        {3.84375F, 3.8125F, 3.5625F, 1.0F},
        {3.46875F, 3.53125F, 3.25F, 1.0F},
        {2.03125F, 0.6875F, 0.53125F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {2.0F, 2.0F, 2.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F}
    }};
    static constexpr std::array<Float4, kWidth> chromatic{{
        {8.0F, 0.0F, 0.0F, 1.0F},
        {0.0F, 8.0F, 0.0F, 1.0F},
        {0.0F, 0.0F, 8.0F, 1.0F},
        {8.0F, 8.0F, 0.0F, 1.0F},
        {0.0F, 8.0F, 8.0F, 1.0F},
        {8.0F, 0.0F, 8.0F, 1.0F},
        {2.03125F, 0.6875F, 0.53125F, 1.0F},
        {0.0F, 0.0F, 0.5F, 1.0F}
    }};
    inline static const std::array<Float4, kWidth> invalid{{
        {NAN, 0.0F, 0.0F, 1.0F},
        {0.0F, NAN, 0.0F, 1.0F},
        {0.0F, 0.0F, NAN, 1.0F},
        {INFINITY, 0.0F, 0.0F, 1.0F},
        {0.0F, INFINITY, 0.0F, 1.0F},
        {0.0F, 0.0F, INFINITY, 1.0F},
        {-INFINITY, -1.0F, 0.5F, 1.0F},
        {NAN, -INFINITY, INFINITY, 1.0F}
    }};
    ComPtr<ID3D11ComputeShader> composeShader, outputShader;
    Texture ui, composition, output;
    std::array<float, 3> peakHighlight{};
    std::size_t peakIndex{};
    Texture opaqueUi, uiScene;
    explicit Gt7OperatorUi(Device& source) : device(source) {}
    bool PairwiseCase(const Float4& firstPixel, float peak, std::size_t index) const
    {
        return peak == 1000.0F && firstPixel.red == 8.0F && firstPixel.green == 0.0F && index == 6;
    }
    void Initialize()
    {
        composeShader = CreateShader(
            *device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);
        outputShader = CreateShader(
            *device.device.Get(), fo4hdr::generated::test::kTransferEvaluationCs);
        std::array<Float4, kWidth> transparent{};
        const auto uiPixels = MakeFp16Pixels(transparent);
        D3D11_SUBRESOURCE_DATA uiData{
            uiPixels.data(), static_cast<UINT>(sizeof(uiPixels)), 0};
        ui = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &uiData);
        composition = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        output = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_UNORDERED_ACCESS);

    }
    void CompareAlternatives(std::uint16_t value, std::size_t channel, fo4hdr::LinearRgb input, float peak)
    {
        const std::array alternatives{
            MapProductionOperator(
                TestedToneMapper::aces2Lmt, input, peak),
            fo4hdr::MapAgxToBt2020Nits(input, peak)};
        for (std::size_t alternative = 0;
             alternative < alternatives.size(); ++alternative)
        {
            const std::array channels{
                alternatives[alternative].red,
                alternatives[alternative].green,
                alternatives[alternative].blue};
            const auto otherHalf = FloatToBinary16(
                static_cast<float>(channels[channel]));
            gt7PairwiseDifferences[alternative] =
                gt7PairwiseDifferences[alternative] ||
                CodeDistance(
                    value, otherHalf) > 8U;
        }
    }
    Gt7Nits CheckFp16(const std::array<std::uint16_t, 4>* values, const auto& pixels, const Float4& firstPixel, float peak)
    {
        Gt7Nits result;
        auto& [actualNits, referenceNits] = result;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const fo4hdr::LinearRgb input{
                Binary16ToFloat(pixels[index][0]),
                Binary16ToFloat(pixels[index][1]),
                Binary16ToFloat(pixels[index][2])};
            const auto expected = fo4hdr::MapGt7ToBt2020Nits(input, peak);
            const std::array expectedChannels{
                expected.red, expected.green, expected.blue};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto actual = Binary16ToFloat(values[index][channel]);
                const auto expectedHalf = FloatToBinary16(
                    static_cast<float>(expectedChannels[channel]));
                FO4HDR_WARP_CHECK(std::isfinite(actual));
                FO4HDR_WARP_CHECK(actual >= 0.0F);
                FO4HDR_WARP_CHECK(actual <= peak + 1.0F);
                const auto distance = CodeDistance(
                    values[index][channel], expectedHalf);
                const auto propagatedBound =
                    Gt7Bt2020ErrorBound(expected, channel);
                const auto acceptable =
                    expectedChannels[channel] <= 1.0e-5
                        ? actual <= 1.0e-5F
                        : std::abs(static_cast<double>(actual) -
                              expectedChannels[channel]) <= propagatedBound;
                if (!acceptable) {
                    std::cerr << algorithmName
                              << " FP16 parity: peak=" << peak
                              << " sample=" << index
                              << " channel=" << channel
                              << " input=(" << input.red << ','
                              << input.green << ',' << input.blue << ')'
                              << " actual=" << actual
                              << " expected=" << expectedChannels[channel]
                              << " half_ulp=" << distance
                              << " propagated_nits=" << propagatedBound
                              << '\n';
                }
                FO4HDR_WARP_CHECK(acceptable);

                if (PairwiseCase(firstPixel, peak, index))
                    CompareAlternatives(values[index][channel], channel, input, peak);
            }
            actualNits[index] = {
                Binary16ToFloat(values[index][0]),
                Binary16ToFloat(values[index][1]),
                Binary16ToFloat(values[index][2])};
            referenceNits[index] = {
                Binary16ToFloat(FloatToBinary16(
                    static_cast<float>(expected.red))),
                Binary16ToFloat(FloatToBinary16(
                    static_cast<float>(expected.green))),
                Binary16ToFloat(FloatToBinary16(
                    static_cast<float>(expected.blue)))};
        }
        return result;
    }
    void CheckEncoded(const std::uint32_t* encoded, const auto& referenceNits)
    {
        for (std::size_t index = 0; index < referenceNits.size(); ++index) {
            const auto pq = fo4hdr::EncodeSt2084Pq(
                referenceNits[index]);
            const std::array expected{
                fo4hdr::QuantizeR10Unorm(pq.red),
                fo4hdr::QuantizeR10Unorm(pq.green),
                fo4hdr::QuantizeR10Unorm(pq.blue)};
            const std::array<std::uint16_t, 3> actual{
                static_cast<std::uint16_t>(encoded[index] & 0x3FFU),
                static_cast<std::uint16_t>(
                    (encoded[index] >> 10U) & 0x3FFU),
                static_cast<std::uint16_t>(
                    (encoded[index] >> 20U) & 0x3FFU)};
            for (std::size_t channel = 0; channel < actual.size(); ++channel)
                FO4HDR_WARP_CHECK(
                    CodeDistance(actual[channel], expected[channel]) <= 1U);
        }
    }
    void CheckCorpus(const std::array<Float4, kWidth>& corpus, float peak)
    {
        const auto pixels = MakeFp16Pixels(corpus);
        D3D11_SUBRESOURCE_DATA sceneData{
            pixels.data(), static_cast<UINT>(sizeof(pixels)), 0};
        auto scene = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &sceneData);
        const auto constants = CreateConstantBuffer(
            *device.device.Get(), CompositionConstants{
                kWidth, kHeight, compositionMode,
                203.0F, 203.0F, peak, 0U, 0U});
        std::array<ID3D11ShaderResourceView*, 4> inputs{
            scene.srv.Get(), nullptr, nullptr, ui.srv.Get()};
        Dispatch(*device.context.Get(), { composeShader.Get(),
            constants.Get(), inputs.data(),
            static_cast<UINT>(inputs.size()), composition.uav.Get()});
        device.context->CopyResource(
            composition.staging.Get(), composition.texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(composition.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            "Map production-operator FP16 output");
        const auto* values =
            static_cast<const std::array<std::uint16_t, 4>*>(mapped.pData);
        const auto nits = CheckFp16(values, pixels, corpus[0], peak);
        const auto& [actualNits, referenceNits] = nits;
        if (corpus[0].red == 0.0F)
            peakHighlight[peakIndex] =
                static_cast<float>(actualNits[6].red);
        device.context->Unmap(composition.staging.Get(), 0);

        const auto outputConstants = CreateConstantBuffer(
            *device.device.Get(),
            OutputConstants{kWidth, kHeight, 1.0F, 0U});
        ID3D11ShaderResourceView* encodedInput = composition.srv.Get();
        Dispatch(*device.context.Get(), { outputShader.Get(),
            outputConstants.Get(), &encodedInput, 1, output.uav.Get()});
        device.context->CopyResource(
            output.staging.Get(), output.texture.Get());
        CheckHresult(device.context->Map(output.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            "Map production-operator HDR10 output");
        const auto* encoded =
            static_cast<const std::uint32_t*>(mapped.pData);
        CheckEncoded(encoded, referenceNits);
        device.context->Unmap(output.staging.Get(), 0);
    }
    void InitializeUi()
    {
        std::array<Float4, kWidth> whiteOverlay{};
        whiteOverlay.fill({1.0F, 1.0F, 1.0F, 1.0F});
        const auto opaquePixels = MakeFp16Pixels(whiteOverlay);
        D3D11_SUBRESOURCE_DATA opaqueData{
            opaquePixels.data(), static_cast<UINT>(sizeof(opaquePixels)), 0};
        opaqueUi = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &opaqueData);
        const auto neutralPixels = MakeFp16Pixels(neutral);
        D3D11_SUBRESOURCE_DATA neutralData{
            neutralPixels.data(), static_cast<UINT>(sizeof(neutralPixels)), 0};
        uiScene = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &neutralData);
    }
    void CheckUiWhite()
    {
        for (const auto peak : {400.0F, 1000.0F, 2000.0F}) {
            for (const auto white : {100.0F, 203.0F, 300.0F}) {
                const auto constants = CreateConstantBuffer(
                    *device.device.Get(), CompositionConstants{
                        kWidth, kHeight, compositionMode,
                        white, 203.0F, peak, 0U, 0U});
                std::array<ID3D11ShaderResourceView*, 4> inputs{
                    uiScene.srv.Get(), nullptr, nullptr, opaqueUi.srv.Get()};
                Dispatch(*device.context.Get(), { composeShader.Get(),
                    constants.Get(), inputs.data(),
                    static_cast<UINT>(inputs.size()), composition.uav.Get()});
                device.context->CopyResource(
                    composition.staging.Get(), composition.texture.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                CheckHresult(device.context->Map(composition.staging.Get(), 0,
                    D3D11_MAP_READ, 0, &mapped),
                    "Map independent production UI paper white");
                const auto* values = static_cast<const std::uint16_t*>(mapped.pData);
                for (std::size_t channel = 0; channel < 3; ++channel)
                    FO4HDR_WARP_CHECK(std::abs(
                        Binary16ToFloat(values[channel]) - white) <= 2.0F);
                device.context->Unmap(composition.staging.Get(), 0);
            }
        }

    }
    void CheckUiBaseline(const std::array<std::uint16_t, 4>* values,
        std::array<std::array<std::uint16_t, 3>, kWidth>& baseline, bool capturedBaseline)
    {
        for (std::size_t index = 0; index < kWidth; ++index) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (!capturedBaseline)
                    baseline[index][channel] = values[index][channel];
                else
                    FO4HDR_WARP_CHECK(
                        values[index][channel] ==
                        baseline[index][channel]);
            }
        }
    }
    void CheckUiIndependence()
    {
        {
            for (const auto peak : {400.0F, 1000.0F, 2000.0F}) {
                std::array<std::array<std::uint16_t, 3>, kWidth> baseline{};
                bool capturedBaseline{};
                for (const auto white : {100.0F, 203.0F, 300.0F}) {
                    const auto constants = CreateConstantBuffer(
                        *device.device.Get(), CompositionConstants{
                            kWidth, kHeight, compositionMode,
                            white, 203.0F, peak, 0U, 0U});
                    std::array<ID3D11ShaderResourceView*, 4> inputs{
                        uiScene.srv.Get(), nullptr, nullptr, ui.srv.Get()};
                    Dispatch(*device.context.Get(), { composeShader.Get(),
                        constants.Get(), inputs.data(),
                        static_cast<UINT>(inputs.size()), composition.uav.Get()});
                    device.context->CopyResource(
                        composition.staging.Get(), composition.texture.Get());

                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    CheckHresult(device.context->Map(composition.staging.Get(), 0,
                        D3D11_MAP_READ, 0, &mapped),
                        "Map GT7 scene independence from UI paper white");
                    const auto* values = static_cast<
                        const std::array<std::uint16_t, 4>*>(mapped.pData);
                    CheckUiBaseline(values, baseline, capturedBaseline);
                    device.context->Unmap(composition.staging.Get(), 0);
                    capturedBaseline = true;
                }
            }
        }
    }
    void Run()
    {
        Initialize();
        for (const auto peak : {400.0F, 1000.0F, 2000.0F}) {
            for (const auto& corpus : {neutral, natural, chromatic, invalid}) CheckCorpus(corpus, peak);
            ++peakIndex;
        }
        FO4HDR_WARP_CHECK(peakHighlight[0] > 203.0F);
        FO4HDR_WARP_CHECK(peakHighlight[0] < peakHighlight[1]);
        FO4HDR_WARP_CHECK(peakHighlight[1] < peakHighlight[2]);
        for (const auto distinct : gt7PairwiseDifferences) FO4HDR_WARP_CHECK(distinct);
        InitializeUi();
        CheckUiWhite();
        CheckUiIndependence();
    }
};

void VerifyGt7OperatorAndUi(Device& device)
{
    Gt7OperatorUi scenario(device);
    scenario.Run();
}

}  // namespace fo4hdr::tests::gpu
