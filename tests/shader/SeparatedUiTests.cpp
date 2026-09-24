// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace fo4hdr::tests::gpu {

class SeparatedUiFixture
{
public:
    SeparatedUiFixture(Device& gpu, TestedToneMapper algorithm) :
        device(gpu), toneMapper(algorithm) {}

    void Verify()
    {
        Initialize();
        for (const auto peakValue : {400.0F, 1000.0F, 2000.0F}) {
            peak = peakValue;
            baselineScene = {};
            capturedBaseline = false;
            for (const auto whiteValue : {100.0F, 203.0F, 300.0F}) {
                white = whiteValue;
                VerifyHdrWhite();
            }

        }

        FO4HDR_WARP_CHECK(preservedAboveOne);
        VerifySdrFallback();
    }

private:
    // Fallout 4's late UI pass writes straight sRGB into an
    // R8 target with native premultiplied blending. Production must decode
    // that separate target before composing it over the linear scene.
    static constexpr std::array<Float4, kWidth> sourceScene{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {8.0F, 4.0F, 2.0F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {2.0F, 0.5F, 0.125F, 1.0F},
        {0.125F, 2.0F, 0.5F, 1.0F},
        {0.5F, 0.125F, 2.0F, 1.0F},
        {4.0F, 4.0F, 4.0F, 1.0F},
        {8.0F, 8.0F, 8.0F, 1.0F},
    }};
    static constexpr std::array<Float4, kWidth> straightGameUi{{
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {1.0F, 0.0F, 0.0F, 0.25F},
        {0.0F, 1.0F, 0.0F, 0.5F},
        {0.0F, 0.0F, 1.0F, 1.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {1.0F, 1.0F, 1.0F, 0.5F},
    }};
    static constexpr std::array<Float4, kWidth> encodedPremultipliedImGui{{
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.25F, 0.0F, 0.25F},
        {0.0F, 0.0F, 0.5F, 0.5F},
        {0.25F, 0.0F, 0.0F, 0.5F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }};
    static constexpr std::array<Float4, kWidth> straightSdrCodeBackdrop{{
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.21350764F, 0.21350764F, 0.16993466F, 0.06547177F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
    }};
    Device& device;
    TestedToneMapper toneMapper;
    std::array<std::array<std::uint8_t, 4>, kWidth> gameUiPixels{};
    Texture gameUi;
    std::array<std::array<std::uint16_t, 4>, kWidth> backdropPixels{};
    Texture backdrop;
    std::array<std::array<std::uint16_t, 4>, kWidth> imguiPixels{};
    Texture imgui;
    Texture composition;
    Texture output;
    ComPtr<ID3D11ComputeShader> composeShader;
    ComPtr<ID3D11ComputeShader> outputShader;
    ComPtr<ID3D11Buffer> outputConstants;
    bool preservedAboveOne{};
    float peak{};
    float white{};
    std::array<double, 3> baselineScene{};
    bool capturedBaseline{};
    std::array<Float4, kWidth> BuildProxySamples()
    {
        std::array<Float4, kWidth> proxySamples{};
        for (std::size_t index = 0; index < proxySamples.size(); ++index) {
            const auto& original = sourceScene[index];
            const auto sceneNits = MapProductionOperator(
                toneMapper,
                {original.red, original.green, original.blue}, peak);
            const std::array sceneChannels{
                sceneNits.red, sceneNits.green, sceneNits.blue};
            std::array<float, 3> proxyChannels{};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto relativeScene =
                    sceneChannels[channel] / static_cast<double>(white);
                proxyChannels[channel] =
                    static_cast<float>(relativeScene);

                // FP16 proxy storage may shift the fixed-203-nit temporal
                // history by one R8 code, but never changes its scale or
                // makes the temporal codec depend on UI paper white.
                const auto stored = Binary16ToFloat(
                    FloatToBinary16(static_cast<float>(relativeScene)));
                const auto compensated =
                    static_cast<double>(stored) * white /
                    kReferenceWhiteNits;
                const auto fixedReference =
                    sceneChannels[channel] / kReferenceWhiteNits;
                const auto actualHistory = TemporalHistoryCode(
                    fo4hdr::EncodeTemporalIntermediate(compensated));
                const auto expectedHistory = TemporalHistoryCode(
                    fo4hdr::EncodeTemporalIntermediate(fixedReference));
                FO4HDR_WARP_CHECK(
                    actualHistory > expectedHistory
                        ? actualHistory - expectedHistory <= 1U
                        : expectedHistory - actualHistory <= 1U);
                const auto decoded = fo4hdr::DecodeTemporalIntermediate(
                    fo4hdr::EncodeTemporalIntermediate(compensated)) *
                    kReferenceWhiteNits / white;
                FO4HDR_WARP_CHECK(std::abs(
                    decoded - static_cast<double>(stored)) <=
                    std::max(1.0e-9,
                        static_cast<double>(stored) * 1.0e-9));
            }

            proxySamples[index] = {
                proxyChannels[0], proxyChannels[1], proxyChannels[2],
                1.0F};
            preservedAboveOne = preservedAboveOne ||
                proxyChannels[0] > 1.0F;
        }

        return proxySamples;
    }

    struct CompositionChannelSample
    {
        std::size_t index;
        std::size_t channel;
        double actual;
        double expected;
        double backdropEncodedPremultiplied;
    };

    void VerifyBlackCompositionSample(const CompositionChannelSample& sample)
    {
        const auto index = sample.index;
        const auto actual = sample.actual;
        if (index == 0 || index == 7) {
            if (toneMapper == TestedToneMapper::agX) {
                // The selected sigmoid has a sub-visible black
                // tail. UI-relative FP16 storage may quantize it
                // to one minimum subnormal; even at the maximum
                // 300-nit UI white this stays below 1.8e-5 nit and
                // the first nonzero PQ/R10 code.
                FO4HDR_WARP_CHECK(actual <= 2.0e-5);
            } else
                FO4HDR_WARP_CHECK(actual == 0.0);
        }

    }
    void VerifySceneBaselineSample(const CompositionChannelSample& sample)
    {
        const auto index = sample.index;
        const auto channel = sample.channel;
        const auto actual = sample.actual;
        if (index == 1) {
            if (!capturedBaseline)
                baselineScene[channel] = actual;
            else
                FO4HDR_WARP_CHECK(std::abs(
                    actual - baselineScene[channel]) <=
                    std::max(2.0,
                        baselineScene[channel] * 0.02));
        }

    }
    void VerifyCompositionChannel(const CompositionChannelSample& sample)
    {
        const auto [index, channel, actual, expected, backdropEncodedPremultiplied] = sample;
        const auto tolerance = std::max(
            2.0, std::abs(expected) * 0.02);
        if (std::abs(actual - expected) > tolerance) {
            std::cerr << "UI composition: operator="
                      << static_cast<int>(toneMapper)
                      << " peak=" << peak
                      << " paper_white=" << white
                      << " sample=" << index
                      << " channel=" << channel
                      << " actual_nits=" << actual
                      << " expected_nits=" << expected << '\n';
        }

        FO4HDR_WARP_CHECK(std::isfinite(actual));
        FO4HDR_WARP_CHECK(actual >= 0.0);
        FO4HDR_WARP_CHECK(
            std::abs(actual - expected) <= tolerance);
        if (index == 3 && channel == 0)
            FO4HDR_WARP_CHECK(
                DecodeSrgb(backdropEncodedPremultiplied) <
                backdropEncodedPremultiplied * 0.1);
        VerifyBlackCompositionSample(sample);

        if (index == 2 || index == 6)
            FO4HDR_WARP_CHECK(
                std::abs(actual - white) <= 2.0);
        VerifySceneBaselineSample(sample);
    }

    struct DecodedUiLayers
    {
        double backdropAlpha;
        double gameAlpha;
        double imguiAlpha;
        std::array<double, 3> proxyValues;
        std::array<double, 3> backdropValues;
        std::array<double, 3> gameUiValues;
        std::array<double, 3> imguiValues;
    };

    DecodedUiLayers ReadUiReferenceLayers(std::size_t index,
        const std::array<std::array<std::uint16_t, 4>, kWidth>& proxyPixels)
    {
        const auto backdropAlpha = static_cast<double>(
            Binary16ToFloat(backdropPixels[index][3]));
        const auto gameAlpha =
            static_cast<double>(gameUiPixels[index][3]) / 255.0;
        const auto imguiAlpha = static_cast<double>(
            Binary16ToFloat(imguiPixels[index][3]));
        fo4hdr::LinearRgb proxyNits{
            Binary16ToFloat(proxyPixels[index][0]) * white,
            Binary16ToFloat(proxyPixels[index][1]) * white,
            Binary16ToFloat(proxyPixels[index][2]) * white};
        std::array<double, 3> backdropBt709Channels{};
        std::array<double, 3> gameUiBt709Channels{};
        std::array<double, 3> imguiBt709Channels{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto backdropEncodedPremultiplied =
                static_cast<double>(Binary16ToFloat(
                    backdropPixels[index][channel]));
            backdropBt709Channels[channel] =
                DecodeSrgb(std::max(
                    backdropEncodedPremultiplied, 0.0)) * white;
            const auto gameEncodedPremultiplied =
                static_cast<double>(gameUiPixels[index][channel]) /
                255.0;
            const auto gameStraight = gameAlpha > 0.000001
                ? gameEncodedPremultiplied / gameAlpha
                : 0.0;
            gameUiBt709Channels[channel] =
                DecodeSrgb(gameStraight) * gameAlpha * white;
            const auto imguiEncodedPremultiplied =
                static_cast<double>(Binary16ToFloat(
                    imguiPixels[index][channel]));
            const auto imguiStraight = imguiAlpha > 0.000001
                ? imguiEncodedPremultiplied / imguiAlpha
                : 0.0;
            imguiBt709Channels[channel] =
                DecodeSrgb(imguiStraight) * imguiAlpha * white;
        }

        const auto backdropBt2020 =
            fo4hdr::ConvertBt709ToBt2020({
                backdropBt709Channels[0],
                backdropBt709Channels[1],
                backdropBt709Channels[2]});
        const auto gameUiBt2020 =
            fo4hdr::ConvertBt709ToBt2020({
                gameUiBt709Channels[0], gameUiBt709Channels[1],
                gameUiBt709Channels[2]});
        const auto imguiBt2020 =
            fo4hdr::ConvertBt709ToBt2020({
                imguiBt709Channels[0], imguiBt709Channels[1],
                imguiBt709Channels[2]});
        const std::array proxyValues{
            proxyNits.red, proxyNits.green, proxyNits.blue};
        const std::array backdropValues{backdropBt2020.red,
            backdropBt2020.green, backdropBt2020.blue};
        const std::array gameUiValues{gameUiBt2020.red,
            gameUiBt2020.green, gameUiBt2020.blue};
        const std::array imguiValues{imguiBt2020.red,
            imguiBt2020.green, imguiBt2020.blue};
        return {backdropAlpha, gameAlpha, imguiAlpha, proxyValues,
            backdropValues, gameUiValues, imguiValues};
    }

    std::array<fo4hdr::LinearRgb, kWidth> ReadComposedNits(
        const std::array<std::uint16_t, 4>* values,
        const std::array<std::array<std::uint16_t, 4>, kWidth>& proxyPixels)
    {
        std::array<fo4hdr::LinearRgb, kWidth> composedNits{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto [backdropAlpha, gameAlpha, imguiAlpha, proxyValues,
                backdropValues, gameUiValues, imguiValues] =
                ReadUiReferenceLayers(index, proxyPixels);
            std::array<double, 3> actualChannels{};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto backdropEncodedPremultiplied =
                    static_cast<double>(Binary16ToFloat(
                        backdropPixels[index][channel]));
                const auto afterBackdrop = backdropValues[channel] +
                    proxyValues[channel] * (1.0 - backdropAlpha);
                const auto afterGameUi = gameUiValues[channel] +
                    afterBackdrop * (1.0 - gameAlpha);
                const auto expected = imguiValues[channel] +
                    afterGameUi * (1.0 - imguiAlpha);
                const auto actual = static_cast<double>(
                    Binary16ToFloat(values[index][channel]));
                actualChannels[channel] = actual;
                VerifyCompositionChannel({index, channel, actual, expected,
                    backdropEncodedPremultiplied});
            }

            composedNits[index] = {
                actualChannels[0], actualChannels[1], actualChannels[2]};
        }

        return composedNits;
    }

    void VerifyEncodedOutput(const std::uint32_t* encoded,
        const std::array<fo4hdr::LinearRgb, kWidth>& composedNits)
    {
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto pq = fo4hdr::EncodeSt2084Pq(
                composedNits[index]);
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
            for (std::size_t channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(
                    CodeDistance(actual[channel], expected[channel]) <=
                    1U);
            FO4HDR_WARP_CHECK((encoded[index] >> 30U) == 3U);
        }

    }

    void VerifySdrPixels(const std::array<std::uint16_t, 4>* sdrValues,
        const std::array<std::array<std::uint16_t, 4>, kWidth>& sdrScenePixels)
    {
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto backdropAlpha = static_cast<double>(
                Binary16ToFloat(backdropPixels[index][3]));
            const auto alpha =
                static_cast<double>(gameUiPixels[index][3]) / 255.0;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto scene = static_cast<double>(Binary16ToFloat(
                    sdrScenePixels[index][channel]));
                const auto backdropPremultiplied = static_cast<double>(
                    Binary16ToFloat(backdropPixels[index][channel]));
                const auto afterBackdrop = backdropPremultiplied +
                    scene * (1.0 - backdropAlpha);
                const auto uiEncodedPremultiplied =
                    static_cast<double>(gameUiPixels[index][channel]) / 255.0;
                const auto expected =
                    uiEncodedPremultiplied + afterBackdrop * (1.0 - alpha);
                const auto actual = static_cast<double>(Binary16ToFloat(
                    sdrValues[index][channel]));
                FO4HDR_WARP_CHECK(std::abs(actual - expected) <= 0.002);
            }

        }

    }

    static std::array<Float4, kWidth> PremultiplyUi(
        const std::array<Float4, kWidth>& straight)
    {
        std::array<Float4, kWidth> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            const auto& source = straight[index];
            result[index] = {
                source.red * source.alpha,
                source.green * source.alpha,
                source.blue * source.alpha,
                source.alpha};
        }

        return result;
    }

    void Initialize()
    {
        const auto encodedPremultipliedGameUi = PremultiplyUi(straightGameUi);
        gameUiPixels = MakeUnorm8Pixels(
            encodedPremultipliedGameUi);
        D3D11_SUBRESOURCE_DATA gameUiData{
            gameUiPixels.data(), static_cast<UINT>(sizeof(gameUiPixels)), 0};
        gameUi = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE, &gameUiData);
        const auto encodedPremultipliedBackdrop = PremultiplyUi(straightSdrCodeBackdrop);
        backdropPixels = MakeFp16Pixels(
            encodedPremultipliedBackdrop);
        D3D11_SUBRESOURCE_DATA backdropData{
            backdropPixels.data(), static_cast<UINT>(sizeof(backdropPixels)), 0};
        backdrop = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &backdropData);
        imguiPixels = MakeFp16Pixels(encodedPremultipliedImGui);
        D3D11_SUBRESOURCE_DATA imguiData{
            imguiPixels.data(), static_cast<UINT>(sizeof(imguiPixels)), 0};
        imgui = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &imguiData);
        composition = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        output = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_UNORDERED_ACCESS);
        composeShader = CreateShader(
            *device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);
        outputShader = CreateShader(
            *device.device.Get(), fo4hdr::generated::test::kTransferEvaluationCs);
        outputConstants = CreateConstantBuffer(
            *device.device.Get(),
            OutputConstants{kWidth, kHeight, 1.0F, 0U});
    }

    void VerifyHdrWhite()
    {
        const auto proxySamples = BuildProxySamples();
        const auto proxyPixels = MakeFp16Pixels(proxySamples);
        D3D11_SUBRESOURCE_DATA proxyData{
            proxyPixels.data(), static_cast<UINT>(sizeof(proxyPixels)), 0};
        auto proxy = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &proxyData);
        const auto constants = CreateConstantBuffer(
            *device.device.Get(), CompositionConstants{
                kWidth, kHeight, 3U,
                white, kReferenceWhiteNits, peak, 1U, 1U});
        std::array<ID3D11ShaderResourceView*, 4> inputs{
            proxy.srv.Get(), backdrop.srv.Get(), gameUi.srv.Get(),
            imgui.srv.Get()};
        Dispatch(*device.context.Get(), { composeShader.Get(),
            constants.Get(), inputs.data(),
            static_cast<UINT>(inputs.size()), composition.uav.Get()});
        device.context->CopyResource(
            composition.staging.Get(), composition.texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(composition.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            "Map separated native UI and ImGui composition");
        const auto* values = static_cast<
            const std::array<std::uint16_t, 4>*>(mapped.pData);
        const auto composedNits = ReadComposedNits(values, proxyPixels);
        device.context->Unmap(composition.staging.Get(), 0);
        capturedBaseline = true;

        ID3D11ShaderResourceView* composedInput = composition.srv.Get();
        Dispatch(*device.context.Get(), { outputShader.Get(),
            outputConstants.Get(), &composedInput, 1, output.uav.Get()});
        device.context->CopyResource(
            output.staging.Get(), output.texture.Get());
        CheckHresult(device.context->Map(output.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            "Map separated native UI and ImGui HDR10 output");
        const auto* encoded =
            static_cast<const std::uint32_t*>(mapped.pData);
        VerifyEncodedOutput(encoded, composedNits);
        device.context->Unmap(output.staging.Get(), 0);
    }

    void VerifySdrFallback()
    {
        // The bounded SDR fallback recomposes captured native UI in its original
        // gamma-domain premultiplied form and never applies the ImGui overlay.
        constexpr std::array<Float4, kWidth> sdrScene{{
            {0.2F, 0.4F, 0.6F, 1.0F}, {0.1F, 0.2F, 0.3F, 1.0F},
            {0.8F, 0.7F, 0.6F, 1.0F}, {0.4F, 0.4F, 0.4F, 1.0F},
            {0.3F, 0.5F, 0.7F, 1.0F}, {0.9F, 0.2F, 0.1F, 1.0F},
            {0.25F, 0.5F, 0.75F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F},
        }};
        const auto sdrScenePixels = MakeFp16Pixels(sdrScene);
        D3D11_SUBRESOURCE_DATA sdrSceneData{
            sdrScenePixels.data(), static_cast<UINT>(sizeof(sdrScenePixels)), 0};
        auto sdrSceneTexture = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &sdrSceneData);
        const auto sdrConstants = CreateConstantBuffer(
            *device.device.Get(), CompositionConstants{
                kWidth, kHeight, 0U, 203.0F, 203.0F, 1000.0F, 1U, 1U});
        std::array<ID3D11ShaderResourceView*, 4> sdrInputs{
            sdrSceneTexture.srv.Get(), backdrop.srv.Get(), gameUi.srv.Get(),
            imgui.srv.Get()};
        Dispatch(*device.context.Get(), { composeShader.Get(), sdrConstants.Get(),
            sdrInputs.data(), static_cast<UINT>(sdrInputs.size()),
            composition.uav.Get()});
        device.context->CopyResource(
            composition.staging.Get(), composition.texture.Get());
        D3D11_MAPPED_SUBRESOURCE sdrMapped{};
        CheckHresult(device.context->Map(composition.staging.Get(), 0,
            D3D11_MAP_READ, 0, &sdrMapped), "Map native UI SDR fallback");
        const auto* sdrValues = static_cast<
            const std::array<std::uint16_t, 4>*>(sdrMapped.pData);
        VerifySdrPixels(sdrValues, sdrScenePixels);
        device.context->Unmap(composition.staging.Get(), 0);
    }

};

void VerifySeparatedUiProductionPath(Device& device, TestedToneMapper toneMapper)
{
    SeparatedUiFixture fixture(device, toneMapper);
    fixture.Verify();
}

}  // namespace fo4hdr::tests::gpu
