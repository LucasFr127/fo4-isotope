// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "reference/NativeGradeReference.hpp"
#include "reference/ImageControlReference.hpp"
#include "FO4Isotope/tonemap/Aces2.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

namespace fo4hdr::tests::gpu {

struct ImageControlArithmeticCheck
{
    const std::array<Float4, kWidth> &corpus;
    const std::array<NativeGradeRgb, kWidth> &actual;
    std::array<float, 2> combination;
    float blacks;
    std::size_t corpusIndex;
    double &maxArithmeticError;

    bool Neutral() const
    {
        return NeutralShadowContrast(combination[0], combination[1]);
    }
    bool BeyondKnee(std::size_t i) const
    {
        return corpusIndex == 1 && i >= 3;
    }

    void CheckChannel(std::size_t i, std::size_t c, const NativeGradeRgb &source, const NativeGradeRgb &reference) const
    {
        const double error = std::abs(actual[i][c] - reference[c]);
        maxArithmeticError = std::max(maxArithmeticError, error);
        const double arithmeticTolerance = 2.0e-7 + 3.0e-6 * reference[c];
        if (error > arithmeticTolerance)
            std::cerr << "Blacks parity source=" << source[0] << ',' << source[1] << ',' << source[2]
                      << " strength=" << blacks << " shadows=" << combination[0] << " contrast=" << combination[1]
                      << " channel=" << c << " actual=" << actual[i][c] << " expected=" << reference[c] << '\n';
        FO4HDR_WARP_CHECK(std::isfinite(actual[i][c]) && actual[i][c] >= 0);
        FO4HDR_WARP_CHECK(error <= arithmeticTolerance);
        if (Neutral() && blacks == 0)
            FO4HDR_WARP_CHECK(actual[i][c] == source[c]);
        if (Neutral() && BeyondKnee(i))
            FO4HDR_WARP_CHECK(actual[i][c] == source[c]);
        if (source[c] == 0)
            FO4HDR_WARP_CHECK(actual[i][c] == 0);
    }

    void CheckMonotonic(std::size_t i, const NativeGradeRgb &reference) const
    {
        if (!Neutral())
            return;
        if (corpusIndex >= 2 || i == 0)
            return;
        FO4HDR_WARP_CHECK(actual[i][0] >= actual[i - 1][0]);
        const double minimumSlope = blacks < 0 ? 0.25 : 0.75;
        const double inputStep = static_cast<double>(corpus[i].red) - corpus[i - 1].red;
        const double roundoff = 2 * (2.0e-7 + 3.0e-6 * reference[0]);
        FO4HDR_WARP_CHECK(actual[i][0] - actual[i - 1][0] >= minimumSlope * inputStep - roundoff);
    }

    void CheckKnee() const
    {
        if (corpusIndex != 1 || !Neutral())
            return;
        // Adjacent secants approach slope 1 on both sides. The small
        // residual is bounded by the polynomial's analytic curvature
        // plus propagated float arithmetic, not a visual-quality limit.
        const double h = static_cast<double>(corpus[3].red) - corpus[1].red;
        const double left = (actual[3][0] - actual[1][0]) / h;
        const double right = (actual[5][0] - actual[3][0]) / (static_cast<double>(corpus[5].red) - corpus[3].red);
        const double pointTolerance = 2.0e-7 + 3.0e-6 * corpus[5].red;
        const double secantRoundoff = 4 * pointTolerance / h;
        FO4HDR_WARP_CHECK(std::abs(left - right) <= 3 * std::abs(blacks) * h / 0.02 + secantRoundoff);
    }

    void Verify() const
    {
        for (std::size_t i = 0; i < kWidth; ++i) {
            const NativeGradeRgb source{corpus[i].red, corpus[i].green, corpus[i].blue};
            const auto reference = ApplyImageControlsOracle(source, combination[0], combination[1], blacks);
            for (std::size_t c = 0; c < 3; ++c)
                CheckChannel(i, c, source, reference);
            // A scalar toe must preserve mixed-color channel ratios.
            for (std::size_t c = 1; c < 3; ++c)
                FO4HDR_WARP_CHECK(std::abs(actual[i][0] * source[c] - actual[i][c] * source[0]) <=
                                  2.0e-7 + 3.0e-6 * std::abs(actual[i][0] * source[c]));
            CheckMonotonic(i, reference);
        }
        CheckKnee();
    }
};

struct ImageControlProductionCheck
{
    TestedToneMapper mapper;
    float blacks, gradeStrength;
    bool nearBlackCorpus;
    const fo4hdr::Aces2TransformHandle &aces;
    const std::array<std::array<std::uint16_t, 4>, kWidth> &pixels;

    bool ExactBlack(std::size_t i) const
    {
        return nearBlackCorpus && i < 2 && gradeStrength == 0;
    }
    bool ReportNearZero(std::size_t i) const
    {
        return nearBlackCorpus && i >= 2 && gradeStrength == 0 && blacks == 0;
    }

    std::array<fo4hdr::LinearRgb, kWidth> ReadNits(ID3D11DeviceContext &context, Texture &composition) const
    {
        context.CopyResource(composition.staging.Get(), composition.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(context.Map(composition.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                     "Map Blacks production nits");
        const auto *half = static_cast<const std::array<std::uint16_t, 4> *>(mapped.pData);
        std::array<fo4hdr::LinearRgb, kWidth> nits{};
        for (std::size_t i = 0; i < kWidth; ++i) {
            const auto graded = FrozenNativeGrade(
                {Binary16ToFloat(pixels[i][0]), Binary16ToFloat(pixels[i][1]), Binary16ToFloat(pixels[i][2])},
                std::array<float, 4>{1, 0, 0, 0}, gradeStrength);
            const auto controlled = ApplyImageControlsOracle(graded, 0, 1, blacks);
            const fo4hdr::LinearRgb source{controlled[0], controlled[1], controlled[2]};
            const auto expected =
                aces ? fo4hdr::MapAces2LmtToBt2020Nits(*aces, source) : MapProductionOperator(mapper, source, 1000);
            const NativeGradeRgb reference{expected.red, expected.green, expected.blue};
            nits[i] = {Binary16ToFloat(half[i][0]), Binary16ToFloat(half[i][1]), Binary16ToFloat(half[i][2])};
            const NativeGradeRgb actual{nits[i].red, nits[i].green, nits[i].blue};
            for (std::size_t c = 0; c < 3; ++c) {
                const double stores = 203 * Binary16LargestAdjacentStep(static_cast<float>(reference[c] / 203)) +
                                      Binary16LargestAdjacentStep(static_cast<float>(reference[c]));
                FO4HDR_WARP_CHECK(std::isfinite(actual[c]) && actual[c] >= 0);
                FO4HDR_WARP_CHECK(std::abs(actual[c] - reference[c]) <= 5 + stores);
                if (ExactBlack(i))
                    FO4HDR_WARP_CHECK(actual[c] == 0);
            }
        }
        context.Unmap(composition.staging.Get(), 0);
        return nits;
    }

    void ReadPq(ID3D11DeviceContext &context, Texture &pqOutput,
                const std::array<fo4hdr::LinearRgb, kWidth> &nits) const
    {
        context.CopyResource(pqOutput.staging.Get(), pqOutput.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(context.Map(pqOutput.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map Blacks PQ output");
        const auto *packed = static_cast<const std::uint32_t *>(mapped.pData);
        for (std::size_t i = 0; i < kWidth; ++i) {
            const auto pq = fo4hdr::EncodeSt2084Pq(nits[i]);
            const NativeGradeRgb channels{pq.red, pq.green, pq.blue};
            for (unsigned c = 0; c < 3; ++c)
                FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>((packed[i] >> (10 * c)) & 1023),
                                               fo4hdr::QuantizeR10Unorm(channels[c])) <= 1);
            if (ExactBlack(i))
                FO4HDR_WARP_CHECK((packed[i] & 0x3fffffffU) == 0);
            if (ReportNearZero(i))
                std::cout << "HDR grading nearzero mode=" << ProductionMode(mapper)
                          << " input=" << Binary16ToFloat(pixels[i][0]) << " nits=" << nits[i].red << ','
                          << nits[i].green << ',' << nits[i].blue << " r10=" << (packed[i] & 1023) << ','
                          << ((packed[i] >> 10) & 1023) << ',' << ((packed[i] >> 20) & 1023) << '\n';
        }
        context.Unmap(pqOutput.staging.Get(), 0);
    }
};

void CheckImageControlDirectOperator(Device &device, Texture &directOperatorOutput, TestedToneMapper mapper,
                                     const std::array<Float4, kWidth> &nearBlack)
{
    device.context->CopyResource(directOperatorOutput.staging.Get(), directOperatorOutput.texture.Get());
    D3D11_MAPPED_SUBRESOURCE directMapped{};
    CheckHresult(device.context->Map(directOperatorOutput.staging.Get(), 0, D3D11_MAP_READ, 0, &directMapped),
                 "Map direct operator exact black");
    const auto *direct = static_cast<const Float4 *>(directMapped.pData);
    for (std::size_t i = 0; i < kWidth; ++i) {
        const NativeGradeRgb value{direct[i].red, direct[i].green, direct[i].blue};
        for (const double channel : value) {
            FO4HDR_WARP_CHECK(std::isfinite(channel) && channel >= 0);
            if (i < 2)
                FO4HDR_WARP_CHECK(channel == 0);
        }
        if (i == 2)
            std::cout << "HDR grading direct smallest-positive mode=" << ProductionMode(mapper)
                      << " input=" << nearBlack[i].red << " nits=" << value[0] << ',' << value[1] << ',' << value[2]
                      << '\n';
    }
    device.context->Unmap(directOperatorOutput.staging.Get(), 0);
}

struct ImageControlDirectCase
{
    TestedToneMapper mapper;
    const Texture3D &lut;
    const std::array<Float4, kWidth> &nearBlack;
    const ComPtr<ID3D11ComputeShader> &directOperatorShader;
    Texture &directOperatorOutput;
};

ToneMapConstants ImageControlProductionSettings(TestedToneMapper mapper, float blacks, float gradeStrength)
{
    const auto algorithm = ProductionAlgorithm(mapper);
    ToneMapConstants settings{
        ProductionMode(mapper),
        1000,
        203,
        203,
        1,
        203,
        static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(algorithm, 1000)),
        static_cast<float>(fo4hdr::OperatorLutInputShaperMaximum(algorithm, 1000))};
    settings.vanillaGradeStrength = gradeStrength;
    settings.blacksStrength = blacks;
    return settings;
}

struct ImageControlFixture
{
    Device &device;
    ID3D11VertexShader *vertexShader;
    const ComPtr<ID3D11PixelShader> &shader;
    const ComPtr<ID3D11Buffer> &lutConstants;
    std::array<ID3D11SamplerState *, 8> &samplers;
    const Texture3D &identity;
    const ComPtr<ID3D11SamplerState> &linear;
    Texture &controlsOutput;
    static constexpr std::array<std::array<Float4, kWidth>, 3> corpora{{{{{0, 0, 0, 1},
                                                                          {0.00000001F, 0.00000001F, 0.00000001F, 1},
                                                                          {0.0001F, 0.0001F, 0.0001F, 1},
                                                                          {0.001F, 0.001F, 0.001F, 1},
                                                                          {0.005F, 0.005F, 0.005F, 1},
                                                                          {0.01F, 0.01F, 0.01F, 1},
                                                                          {0.015F, 0.015F, 0.015F, 1},
                                                                          {0.02F, 0.02F, 0.02F, 1}}},
                                                                        {{{0.0198F, 0.0198F, 0.0198F, 1},
                                                                          {0.0199F, 0.0199F, 0.0199F, 1},
                                                                          {0.01995F, 0.01995F, 0.01995F, 1},
                                                                          {0.02F, 0.02F, 0.02F, 1},
                                                                          {0.02005F, 0.02005F, 0.02005F, 1},
                                                                          {0.0201F, 0.0201F, 0.0201F, 1},
                                                                          {1, 1, 1, 1},
                                                                          {4, 4, 4, 1}}},
                                                                        {{{0.0004F, 0.0001F, 0.000025F, 1},
                                                                          {0.004F, 0.001F, 0.00025F, 1},
                                                                          {0.04F, 0.01F, 0.0025F, 1},
                                                                          {0.001F, 0.004F, 0.016F, 1},
                                                                          {0, 0.005F, 0.02F, 1},
                                                                          {0.08F, 0.02F, 0.005F, 1},
                                                                          {1, 0.25F, 0.0625F, 1},
                                                                          {16, 4, 1, 1}}}}};
    static constexpr std::array<std::array<float, 2>, 5> combinations{
        {{0, 1}, {-2, 0.5F}, {2, 1.5F}, {-1, 1.25F}, {1, 0.75F}}};
    static constexpr std::array<Float4, kWidth> nearBlack{{{0, 0, 0, 1},
                                                    {-1, -1, -1, 1},
                                                    {0.000000059604645F, 0.000000059604645F, 0.000000059604645F, 1},
                                                    {0.00000011920929F, 0.00000011920929F, 0.00000011920929F, 1},
                                                    {0.00000023841858F, 0.00000023841858F, 0.00000023841858F, 1},
                                                    {0.000001F, 0.000001F, 0.000001F, 1},
                                                    {0.00001F, 0.00001F, 0.00001F, 1},
                                                    {0.0001F, 0.0001F, 0.0001F, 1}}};
    std::array<NativeGradeRgb, kWidth> ReadControls() const
    {
        device.context->CopyResource(controlsOutput.staging.Get(), controlsOutput.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(controlsOutput.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                     "Map Blacks image-control output");
        std::array<NativeGradeRgb, kWidth> result{};
        const auto *values = static_cast<const Float4 *>(mapped.pData);
        for (std::size_t i = 0; i < kWidth; ++i) {
            result[i] = {values[i].red, values[i].green, values[i].blue};
            FO4HDR_WARP_CHECK(values[i].alpha == 1);
        }
        device.context->Unmap(controlsOutput.staging.Get(), 0);
        return result;
    }

    double CheckArithmetic() const
    {
        double maxArithmeticError{};
        for (std::size_t corpusIndex = 0; corpusIndex < corpora.size(); ++corpusIndex) {
            const auto &corpus = corpora[corpusIndex];
            D3D11_SUBRESOURCE_DATA data{corpus.data(), sizeof(corpus), 0};
            auto input =
                CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data);
            std::array<ID3D11ShaderResourceView *, 8> resources{
                input.srv.Get(),    nullptr, nullptr, identity.srv.Get(), identity.srv.Get(), identity.srv.Get(),
                identity.srv.Get(), nullptr};
            for (const auto combination : combinations)
                for (const float blacks : {-0.75F, -0.375F, 0.0F, 0.375F, 0.75F}) {
                    ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
                    settings.vanillaGradeStrength = 0;
                    settings.shadowsEv = combination[0];
                    settings.contrastSlope = combination[1];
                    settings.blacksStrength = blacks;
                    const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
                    auto *target = controlsOutput.rtv.Get();
                    DrawFullscreen(*device.context.Get(),
                                   {vertexShader, shader.Get(), lutConstants.Get(), constants.Get(), resources.data(),
                                    8, samplers.data(), 8, &target, 1});
                    const auto actual = ReadControls();
                    ImageControlArithmeticCheck{corpus, actual, combination, blacks, corpusIndex, maxArithmeticError}
                        .Verify();
                }
        }

        return maxArithmeticError;
    }

    void CheckExtremes() const
    {
        // Exercise the finite FP16 working-domain boundaries and source-negative
        // sanitization without treating an artistic toe as a general sanitizer.
        const std::array<Float4, kWidth> extremes{{{-1, -2, -3, 1},
                                                   {65504, 0, 0, 1},
                                                   {0, 65504, 0, 1},
                                                   {0, 0, 65504, 1},
                                                   {65504, 65504, 65504, 1},
                                                   {65504, 1, 0, 1},
                                                   {0.000000059604645F, 0, 0, 1},
                                                   {0, 0, 0, 1}}};
        D3D11_SUBRESOURCE_DATA extremeData{extremes.data(), sizeof(extremes), 0};
        auto extremeInput = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
                                          D3D11_BIND_SHADER_RESOURCE, &extremeData);
        std::array<ID3D11ShaderResourceView *, 8> extremeResources{
            extremeInput.srv.Get(), nullptr, nullptr, identity.srv.Get(), identity.srv.Get(), identity.srv.Get(),
            identity.srv.Get(),     nullptr};
        for (const float blacks : {-0.75F, 0.75F}) {
            ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
            settings.vanillaGradeStrength = 0;
            settings.blacksStrength = blacks;
            const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
            auto *target = controlsOutput.rtv.Get();
            DrawFullscreen(*device.context.Get(), {vertexShader, shader.Get(), lutConstants.Get(), constants.Get(),
                                                   extremeResources.data(), 8, samplers.data(), 8, &target, 1});
            for (const auto &pixel : ReadControls())
                for (const auto channel : pixel)
                    FO4HDR_WARP_CHECK(std::isfinite(channel) && channel >= 0 && channel <= 65504);
        }
    }

    void CheckAdaptation() const
    {
        // Adaptation changes luminance, so this distinguishes the required order
        // (toe first) from a toe accidentally applied to adapted RGB.
        constexpr Float4 adaptationSource{0.01F, 0.0025F, 0.000625F, 1};
        const auto adaptationPixels = UniformPixels(adaptationSource);
        D3D11_SUBRESOURCE_DATA adaptationData{adaptationPixels.data(), sizeof(adaptationPixels), 0};
        auto adaptationInput = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
                                             D3D11_BIND_SHADER_RESOURCE, &adaptationData);
        std::array<ID3D11ShaderResourceView *, 8> extremeResources{
            adaptationInput.srv.Get(), nullptr, nullptr, identity.srv.Get(), identity.srv.Get(), identity.srv.Get(),
            identity.srv.Get(),        nullptr};
        for (const float blacks : {-0.75F, 0.0F, 0.75F}) {
            ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
            settings.vanillaGradeStrength = 0;
            settings.blacksStrength = blacks;
            settings.saturation = 0;
            settings.colorAdaptationRow0.red = 2;
            settings.colorAdaptationRow2.blue = 0.5F;
            const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
            auto *target = controlsOutput.rtv.Get();
            DrawFullscreen(*device.context.Get(), {vertexShader, shader.Get(), lutConstants.Get(), constants.Get(),
                                                   extremeResources.data(), 8, samplers.data(), 8, &target, 1});
            const auto controlled = ApplyImageControlsOracle(
                {adaptationSource.red, adaptationSource.green, adaptationSource.blue}, 0, 1, blacks);
            const double y = controlled[0] * 0.212500006 + controlled[1] * 0.715399981 + controlled[2] * 0.0720999986;
            for (const auto &pixel : ReadControls()) {
                FO4HDR_WARP_CHECK(std::abs(pixel[0] - 2 * y) <= 2.0e-7);
                FO4HDR_WARP_CHECK(std::abs(pixel[1] - y) <= 2.0e-7);
                FO4HDR_WARP_CHECK(std::abs(pixel[2] - 0.5 * y) <= 2.0e-7);
            }
        }
    }

    void CheckDirectOperator(const ImageControlDirectCase &test) const
    {
        const auto &[mapper, lut, directCorpus, directOperatorShader, directOperatorOutput] = test;
        const auto algorithm = ProductionAlgorithm(mapper);
        // Also reach the operator's own sanitizer directly: the complete grade
        // route above it would already have removed negative source channels.
        D3D11_SUBRESOURCE_DATA directData{directCorpus.data(), sizeof(directCorpus), 0};
        auto directInput = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
                                         D3D11_BIND_SHADER_RESOURCE, &directData);
        struct DirectConstants
        {
            std::uint32_t width, count, mode;
            float minimum, maximum, peak;
            std::array<float, 2> padding{};
        };
        const auto directConstants = CreateConstantBuffer(
            *device.device.Get(),
            DirectConstants{kWidth, kWidth, ProductionMode(mapper),
                            static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(algorithm, 1000)),
                            static_cast<float>(fo4hdr::OperatorLutInputShaperMaximum(algorithm, 1000)), 1000});
        std::array<ID3D11ShaderResourceView *, 8> directResources{};
        directResources[0] = directInput.srv.Get();
        directResources[7] = lut.srv.Get();
        auto *directSampler = linear.Get();
        device.context->CSSetSamplers(7, 1, &directSampler);
        Dispatch(*device.context.Get(), {directOperatorShader.Get(), directConstants.Get(), directResources.data(), 8,
                                         directOperatorOutput.uav.Get()});
        directSampler = nullptr;
        device.context->CSSetSamplers(7, 1, &directSampler);
        ID3D11ShaderResourceView *nullSrv{};
        device.context->CSSetShaderResources(7, 1, &nullSrv);
        CheckImageControlDirectOperator(device, directOperatorOutput, mapper, directCorpus);
    }

    void CheckProduction() const
    {
        // Exercise the FP16 grade/operator -> absolute composition -> PQ path
        // with the same control constants at one peak across all three operators.
        auto transport = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        auto composition = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        auto pqOutput = CreateTexture(*device.device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_UNORDERED_ACCESS);
        const auto composeShader = CreateShader(*device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);
        const auto pqShader = CreateShader(*device.device.Get(), fo4hdr::generated::test::kTransferEvaluationCs);
        const auto directOperatorShader = CreateShader(*device.device.Get(), fo4hdr::generated::test::kOperatorLutCs);
        auto directOperatorOutput =
            CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
        const auto composeConstants =
            CreateConstantBuffer(*device.device.Get(), CompositionConstants{kWidth, kHeight, 3, 203, 203, 1000, 0, 0});
        const auto pqConstants = CreateConstantBuffer(*device.device.Get(), OutputConstants{kWidth, kHeight, 1, 0});
        const auto transparent = MakeFp16Pixels(UniformPixels({}));
        D3D11_SUBRESOURCE_DATA uiData{transparent.data(), sizeof(transparent), 0};
        auto ui =
            CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &uiData);
        for (const auto mapper : {TestedToneMapper::aces2Lmt, TestedToneMapper::agX, TestedToneMapper::gt7}) {
            const auto algorithm = ProductionAlgorithm(mapper);
            const auto lut = CreateOperatorLut(*device.device.Get(), algorithm, 1000);
            const auto aces = mapper == TestedToneMapper::aces2Lmt ? fo4hdr::CreateAces2Transform(1000)
                                                                   : fo4hdr::Aces2TransformHandle{};
            FO4HDR_WARP_CHECK(mapper != TestedToneMapper::aces2Lmt || aces);
            CheckDirectOperator({mapper, lut, nearBlack, directOperatorShader, directOperatorOutput});
            for (const auto &corpus : {corpora[0], corpora[1], corpora[2], nearBlack}) {
                const bool nearBlackCorpus = corpus[1].red < 0;
                const auto pixels = MakeFp16Pixels(corpus);
                D3D11_SUBRESOURCE_DATA data{pixels.data(), sizeof(pixels), 0};
                auto scene = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                           D3D11_BIND_SHADER_RESOURCE, &data);
                std::array<ID3D11ShaderResourceView *, 8> resources{scene.srv.Get(),
                                                                    nullptr,
                                                                    nullptr,
                                                                    identity.srv.Get(),
                                                                    identity.srv.Get(),
                                                                    identity.srv.Get(),
                                                                    identity.srv.Get(),
                                                                    lut.srv.Get()};
                for (const float blacks : {-0.75F, 0.0F, 0.75F})
                    for (const float gradeStrength : {0.0F, 1.0F}) {
                        const auto settings = ImageControlProductionSettings(mapper,blacks,gradeStrength);
                        const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
                        auto *target = transport.rtv.Get();
                        DrawFullscreen(*device.context.Get(),
                                       {vertexShader, shader.Get(), lutConstants.Get(), constants.Get(),
                                        resources.data(), 8, samplers.data(), 8, &target, 1});
                        std::array<ID3D11ShaderResourceView *, 4> inputs{transport.srv.Get(), nullptr, nullptr,
                                                                         ui.srv.Get()};
                        Dispatch(*device.context.Get(), {composeShader.Get(), composeConstants.Get(), inputs.data(), 4,
                                                         composition.uav.Get()});
                        const ImageControlProductionCheck check{mapper,          blacks, gradeStrength,
                                                                nearBlackCorpus, aces,   pixels};
                        const auto nits = check.ReadNits(*device.context.Get(), composition);
                        auto *input = composition.srv.Get();
                        Dispatch(*device.context.Get(),
                                 {pqShader.Get(), pqConstants.Get(), &input, 1, pqOutput.uav.Get()});
                        check.ReadPq(*device.context.Get(), pqOutput, nits);
                    }
            }
        }
    }
};

void VerifyImageControls(Device &device, ID3D11VertexShader *vertexShader, ID3D11SamplerState *sceneSampler)
{
    const auto shader = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kCreativeGradeLut);
    const auto identity = CreateIdentityLut(*device.device.Get());
    const auto linear = CreateClampLinearSampler(*device.device.Get());
    LutConstants weights{};
    weights.values[1] = {1, 0, 0, 0};
    const auto lutConstants = CreateConstantBuffer(*device.device.Get(), weights);
    std::array<ID3D11SamplerState *, 8> samplers{sceneSampler, nullptr,      nullptr,      linear.Get(),
                                                 linear.Get(), linear.Get(), linear.Get(), linear.Get()};
    auto controlsOutput = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    const ImageControlFixture fixture{device,   vertexShader, shader, lutConstants,
                                      samplers, identity,     linear, controlsOutput};
    const auto maxArithmeticError = fixture.CheckArithmetic();
    fixture.CheckExtremes();
    fixture.CheckAdaptation();
    fixture.CheckProduction();
    std::cout << "HDR grading Blacks: independent controls max_abs=" << maxArithmeticError
              << "; default bypass, neutral ramps, mixed-color ratios, knee, Shadows/Contrast and three-operator PQ "
                 "checked\n";
}

struct RadialRemovalPixel
{
    Float4 source, baseline, actual;
    std::array<float, 2> removal;
    std::size_t index;
};

void CheckRadialRemovalInvariants(const RadialRemovalPixel &test, const NativeGradeRgb &hdr,
                                  const std::array<float, 3> &channels, const NativeGradeRgb &expected)
{
    constexpr NativeGradeRgb luma{0.212500006, 0.715399981, 0.0720999986};
    const auto dot = [&](const auto &value) { return value[0] * luma[0] + value[1] * luma[1] + value[2] * luma[2]; };
    if (test.removal[1] == 1 && dot(hdr) > 0)
        for (std::size_t channel = 1; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(std::abs(channels[0] * hdr[channel] - channels[channel] * hdr[0]) <=
                              3.0e-5 * std::max(1.0, expected[0]));
    if (test.removal[0] == 1 && test.index < 5)
        FO4HDR_WARP_CHECK(std::abs(dot(channels) - dot(hdr)) <= 3.0e-5 * std::max(1.0, dot(hdr)));
}

void CheckRadialRemovalPixel(const RadialRemovalPixel &test)
{
    const NativeGradeRgb hdr{test.source.red, test.source.green, test.source.blue};
    const NativeGradeRgb graded{test.baseline.red, test.baseline.green, test.baseline.blue};
    const auto expected = ApplyVanillaGradeRemovalOracle(hdr, graded, test.removal[0], test.removal[1]);
    const std::array channels{test.actual.red, test.actual.green, test.actual.blue};
    const std::array baselineChannels{test.baseline.red, test.baseline.green, test.baseline.blue};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto tolerance = 3.0e-5 * std::max(1.0, expected[channel]);
        FO4HDR_WARP_CHECK(std::isfinite(channels[channel]));
        FO4HDR_WARP_CHECK(channels[channel] >= 0.0F && channels[channel] <= 65504.0F);
        FO4HDR_WARP_CHECK(std::abs(channels[channel] - expected[channel]) <= tolerance);
        if (test.removal[0] == 0 && test.removal[1] == 0)
            FO4HDR_WARP_CHECK(channels[channel] == baselineChannels[channel]);
        if (test.removal[0] == 1 && test.removal[1] == 1)
            FO4HDR_WARP_CHECK(channels[channel] == hdr[channel]);
    }
    FO4HDR_WARP_CHECK(test.actual.alpha == test.source.alpha);
    CheckRadialRemovalInvariants(test, hdr, channels, expected);
}

void CheckPureGainRemoval(const std::array<Float4, kWidth> &corpus)
{
    // A pure gain has identical normalized RGB on both sides, so changing the
    // color amount cannot change the result.
    for (std::size_t index = 1; index < kWidth; ++index) {
        const NativeGradeRgb hdr{corpus[index].red, corpus[index].green, corpus[index].blue};
        NativeGradeRgb gained{};
        for (std::size_t channel = 0; channel < 3; ++channel)
            gained[channel] = 0.25 * hdr[channel];
        const auto retained = ApplyVanillaGradeRemovalOracle(hdr, gained, 0.4, 0);
        const auto removed = ApplyVanillaGradeRemovalOracle(hdr, gained, 0.4, 1);
        FO4HDR_WARP_CHECK(retained == removed);
    }
}

template <class Render>
void CheckRadialBlackRemoval(const Render &render, const std::array<Float4, kWidth> &corpus,
                             const std::array<std::array<float, 4>, 3> &looks)
{
    const auto tintedBlack = render(looks[1], 1, 0, 0);
    const auto neutralBlackLift = render(looks[1], 1, 0, 1);
    const auto removedBlackLift = render(looks[1], 1, 1, 1);
    FO4HDR_WARP_CHECK(tintedBlack[0].red > tintedBlack[0].green);
    FO4HDR_WARP_CHECK(tintedBlack[0].green > tintedBlack[0].blue);
    FO4HDR_WARP_CHECK(neutralBlackLift[0].red > 0);
    FO4HDR_WARP_CHECK(std::abs(neutralBlackLift[0].red - neutralBlackLift[0].green) <= 1.0e-6F);
    FO4HDR_WARP_CHECK(std::abs(neutralBlackLift[0].green - neutralBlackLift[0].blue) <= 1.0e-6F);
    FO4HDR_WARP_CHECK(removedBlackLift[0].red == 0 && removedBlackLift[0].green == 0 && removedBlackLift[0].blue == 0);

    const auto crushed = render(looks[2], 1, 0, 0);
    const auto restoredCrush = render(looks[2], 1, 1, 0);
    FO4HDR_WARP_CHECK(crushed[1].red == 0 && crushed[1].green == 0 && crushed[1].blue == 0);
    FO4HDR_WARP_CHECK(std::abs(restoredCrush[1].red - corpus[1].red) <= 1.0e-7F);
    FO4HDR_WARP_CHECK(std::abs(restoredCrush[1].green - corpus[1].green) <= 1.0e-7F);
    FO4HDR_WARP_CHECK(std::abs(restoredCrush[1].blue - corpus[1].blue) <= 1.0e-7F);
}

template <class Render> void CheckRadialRemovalScenarios(const Render &render, const std::array<Float4, kWidth> &corpus)
{
    constexpr std::array<std::array<float, 4>, 3> looks{{{{0.5F, 0, 0, 0}}, {{0, 1, 0, 0}}, {{0, 0, 0, 0}}}};
    constexpr std::array<std::array<float, 2>, 5> removals{{{{0, 0}}, {{0.35F, 0.65F}}, {{1, 0}}, {{0, 1}}, {{1, 1}}}};
    for (const auto &weights : looks)
        for (const float strength : {0.0F, 0.5F, 1.0F}) {
            const auto baseline = render(weights, strength, 0, 0);
            for (const auto removal : removals) {
                const auto actual = render(weights, strength, removal[0], removal[1]);
                for (std::size_t index = 0; index < kWidth; ++index) {
                    CheckRadialRemovalPixel({corpus[index], baseline[index], actual[index], removal, index});
                }
            }
        }

    CheckPureGainRemoval(corpus);

    CheckRadialBlackRemoval(render, corpus, looks);
}

void VerifyRadialVanillaGradeRemoval(Device &device, ID3D11VertexShader *vertexShader, ID3D11SamplerState *sceneSampler)
{
    constexpr std::array<Float4, kWidth> corpus{{{0, 0, 0, 0.125F},
                                                 {0.00001F, 0.0000025F, 0, 0.25F},
                                                 {0.18F, 0.04F, 0.01F, 0.375F},
                                                 {1, 0.25F, 0.0625F, 0.5F},
                                                 {16, 4, 1, 0.625F},
                                                 {65504, 0, 0, 0.75F},
                                                 {0, 65504, 0, 0.875F},
                                                 {0, 0, 65504, 1}}};
    D3D11_SUBRESOURCE_DATA data{corpus.data(), sizeof(corpus), 0};
    const auto scene =
        CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data);
    const auto identity = CreateIdentityLut(*device.device.Get());
    const auto tinted = CreateNativeGrade(*device.device.Get(), 1);
    const auto linear = CreateClampLinearSampler(*device.device.Get());
    const auto shader = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kCreativeGradeLut);
    auto output = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    std::array<ID3D11ShaderResourceView *, 8> resources{scene.srv.Get(),    nullptr,          nullptr,
                                                        identity.srv.Get(), tinted.srv.Get(), identity.srv.Get(),
                                                        identity.srv.Get(), nullptr};
    std::array<ID3D11SamplerState *, 8> samplers{sceneSampler, nullptr,      nullptr,      linear.Get(),
                                                 linear.Get(), linear.Get(), linear.Get(), nullptr};

    const auto render = [&](const std::array<float, 4> &weights, float strength, float luminanceRemoval,
                            float colorRemoval) {
        LutConstants lut{};
        lut.values[1] = {weights[0], weights[1], weights[2], weights[3]};
        const auto lutBuffer = CreateConstantBuffer(*device.device.Get(), lut);
        ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
        settings.vanillaGradeStrength = strength;
        settings.selectiveColorPlaneA.alpha = luminanceRemoval;
        settings.selectiveColorPlaneB.alpha = colorRemoval;
        const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
        auto *target = output.rtv.Get();
        DrawFullscreen(*device.context.Get(), {vertexShader, shader.Get(), lutBuffer.Get(), constants.Get(),
                                               resources.data(), 8, samplers.data(), 8, &target, 1});
        device.context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                     "Map radial grade removal");
        std::array<Float4, kWidth> result{};
        std::copy_n(static_cast<const Float4 *>(mapped.pData), kWidth, result.begin());
        device.context->Unmap(output.staging.Get(), 0);
        return result;
    };

    CheckRadialRemovalScenarios(render, corpus);
}

struct LutProfileRemovalCase
{
    std::array<float, 4> weights;
    std::array<float, 4> luminanceProfiles;
    std::array<float, 4> colorProfiles;
    std::array<float, 2> global;
    bool active{true};
    float strength{1.0F};
};

float MaximumProfileWeight(const std::array<float, 4> &weights)
{
    float maximum{};
    for (const auto weight : weights) {
        if (!std::isfinite(weight) || weight < 0.0F)
            return 0;
        maximum = std::max(maximum, weight);
    }
    return maximum;
}

[[nodiscard]] std::array<float, 2> ResolveLutProfileRemovalOracle(const LutProfileRemovalCase &test)
{
    if (!test.active)
        return test.global;
    const auto maximum = MaximumProfileWeight(test.weights);
    if (!(maximum > 0.0F))
        return test.global;

    std::array<float, 4> scaled{};
    float sum{};
    for (std::size_t slot = 0; slot < scaled.size(); ++slot) {
        scaled[slot] = test.weights[slot] / maximum;
        sum += scaled[slot];
    }
    if (!(sum > 0.0F))
        return test.global;

    std::array<float, 2> result{};
    for (std::size_t slot = 0; slot < scaled.size(); ++slot) {
        const auto normalized = scaled[slot] / sum;
        result[0] += normalized * test.luminanceProfiles[slot];
        result[1] += normalized * test.colorProfiles[slot];
    }
    return result;
}

struct LutProfileReadback
{
    bool nativeMethod;
    std::size_t caseIndex;
    const std::array<float, 2> &expectedRemoval;
    const LutProfileRemovalCase &test;
    const std::array<Float4, kWidth> &corpus;
    const std::array<Float4, kWidth> &actual;
    const std::array<Float4, kWidth> &reference;

    void CheckChannels(std::size_t pixel, bool &mismatchReported) const
    {
        const std::array actualChannels{actual[pixel].red, actual[pixel].green, actual[pixel].blue};
        const std::array referenceChannels{reference[pixel].red, reference[pixel].green, reference[pixel].blue};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto tolerance = 4.0e-5F * std::max(1.0F, std::abs(referenceChannels[channel]));
            const auto difference = std::abs(actualChannels[channel] - referenceChannels[channel]);
            FO4HDR_WARP_CHECK(std::isfinite(actualChannels[channel]));
            if (!mismatchReported && difference > tolerance) {
                mismatchReported = true;
                std::cerr << "LUT profile mismatch method=" << (nativeMethod ? "native" : "radial")
                          << " case=" << caseIndex << " removal=" << expectedRemoval[0] << ',' << expectedRemoval[1]
                          << " pixel=" << pixel << " channel=" << channel << " actual=" << actualChannels[channel]
                          << " reference=" << referenceChannels[channel] << " difference=" << difference
                          << " tolerance=" << tolerance << '\n';
            }
            FO4HDR_WARP_CHECK(difference <= tolerance);
        }
    }

    void Verify() const
    {
        bool mismatchReported{};
        for (std::size_t pixel = 0; pixel < actual.size(); ++pixel) {
            CheckChannels(pixel, mismatchReported);
            FO4HDR_WARP_CHECK(actual[pixel].alpha == reference[pixel].alpha);
            if (test.strength == 0.0F) {
                FO4HDR_WARP_CHECK(actual[pixel].red == corpus[pixel].red);
                FO4HDR_WARP_CHECK(actual[pixel].green == corpus[pixel].green);
                FO4HDR_WARP_CHECK(actual[pixel].blue == corpus[pixel].blue);
            }
        }
    }
};

template <class Render>
void CheckLutProfileScenarios(const Render &render, const std::array<Float4, kWidth> &corpus,
                              const std::array<LutProfileRemovalCase, 14> &cases)
{
    std::array<std::array<float, 2>, 2> permutationResults{};
    std::array<std::array<float, 2>, 2> scaleResults{};
    for (const bool nativeMethod : {false, true})
        for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const auto &test = cases[caseIndex];
            const auto expectedRemoval = ResolveLutProfileRemovalOracle(test);
            const auto actual = render(nativeMethod, test, test.active, test.global);
            const auto reference = render(nativeMethod, test, false, expectedRemoval);
            LutProfileReadback{nativeMethod, caseIndex, expectedRemoval, test, corpus, actual, reference}.Verify();
            if (caseIndex == 4 || caseIndex == 5)
                permutationResults[caseIndex - 4] = expectedRemoval;
            if (caseIndex == 7 || caseIndex == 8)
                scaleResults[caseIndex - 7] = expectedRemoval;
        }

    FO4HDR_WARP_CHECK(permutationResults[0] == permutationResults[1]);
    FO4HDR_WARP_CHECK(scaleResults[0] == scaleResults[1]);
    const auto half = ResolveLutProfileRemovalOracle(cases[3]);
    FO4HDR_WARP_CHECK(half[0] == 0.5F && half[1] == 0.5F);
}

std::array<LutProfileRemovalCase, 14> LutProfileRemovalCases()
{
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto maximum = std::numeric_limits<float>::max();
    const std::array cases{
        LutProfileRemovalCase{
            {0.25F, 0.25F, 0.25F, 0.25F}, {0, 0.25F, 0.75F, 1}, {1, 0.75F, 0.25F, 0}, {0.2F, 0.8F}, false},
        LutProfileRemovalCase{{1, 0, 0, 0}, {0.1F, 0.3F, 0.6F, 0.9F}, {0.9F, 0.6F, 0.3F, 0.1F}, {0.4F, 0.7F}},
        LutProfileRemovalCase{{0, 0, 0, 1}, {0.1F, 0.3F, 0.6F, 0.9F}, {0.9F, 0.6F, 0.3F, 0.1F}, {0.4F, 0.7F}},
        LutProfileRemovalCase{{0.5F, 0.5F, 0, 0}, {0, 1, 0.2F, 0.8F}, {1, 0, 0.8F, 0.2F}, {0.4F, 0.7F}},
        LutProfileRemovalCase{{1, 3, 0, 0}, {0.1F, 0.7F, 0.2F, 0.9F}, {0.9F, 0.3F, 0.8F, 0.1F}, {0.4F, 0.7F}},
        LutProfileRemovalCase{{3, 1, 0, 0}, {0.7F, 0.1F, 0.2F, 0.9F}, {0.3F, 0.9F, 0.8F, 0.1F}, {0.4F, 0.7F}},
        // Slots 0 and 1 represent one duplicate profile; slot 2 represents an
        // unknown LUT populated with the current global values.
        LutProfileRemovalCase{
            {0.2F, 0.3F, 0.4F, 0.1F}, {0.2F, 0.2F, 0.4F, 0.8F}, {0.6F, 0.6F, 0.7F, 0.1F}, {0.4F, 0.7F}},
        LutProfileRemovalCase{{1, 0.5F, 0.25F, 0.125F}, {0, 0.25F, 0.75F, 1}, {1, 0.75F, 0.25F, 0}, {0.4F, 0.7F}},
        LutProfileRemovalCase{{maximum, maximum * 0.5F, maximum * 0.25F, maximum * 0.125F},
                              {0, 0.25F, 0.75F, 1},
                              {1, 0.75F, 0.25F, 0},
                              {0.4F, 0.7F}},
        LutProfileRemovalCase{{0, 0, 0, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}, {0.35F, 0.65F}},
        LutProfileRemovalCase{{nan, 1, 0, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}, {0.35F, 0.65F}},
        LutProfileRemovalCase{{infinity, 1, 0, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}, {0.35F, 0.65F}},
        LutProfileRemovalCase{{-0.25F, 1, 0, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}, {0.35F, 0.65F}},
        LutProfileRemovalCase{{0.25F, 0.25F, 0.25F, 0.25F}, {1, 0, 1, 0}, {0, 1, 0, 1}, {0.35F, 0.65F}, true, 0.0F}};

    return cases;
}

struct LutProfileRenderer
{
    Device &device;
    ID3D11VertexShader *vertexShader;
    ID3D11SamplerState *sceneSampler;
    const Texture &scene;
    const Texture3D &look;
    const ComPtr<ID3D11SamplerState> &linear;
    const ComPtr<ID3D11PixelShader> &radial;
    const ComPtr<ID3D11PixelShader> &native;
    const ComPtr<ID3D11ComputeShader> &prepare;
    Texture &output;
    Texture3D &prepared;
    const ComPtr<ID3D11UnorderedAccessView> &preparedUav;

    void Prepare(ID3D11Buffer *buffer) const
    {
        auto *lutConstants = buffer;
        std::array<ID3D11ShaderResourceView *, 4> looks{look.srv.Get(), look.srv.Get(), look.srv.Get(), look.srv.Get()};
        auto *uav = preparedUav.Get();
        device.context->CSSetShader(prepare.Get(), nullptr, 0);
        device.context->CSSetConstantBuffers(2, 1, &lutConstants);
        device.context->CSSetShaderResources(0, 4, looks.data());
        device.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        device.context->Dispatch(2, 2, 2);
        uav = nullptr;
        lutConstants = nullptr;
        looks.fill(nullptr);
        device.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        device.context->CSSetShaderResources(0, 4, looks.data());
        device.context->CSSetConstantBuffers(2, 1, &lutConstants);
        device.context->CSSetShader(nullptr, nullptr, 0);
    }

    std::array<Float4, kWidth> Render(bool nativeMethod, const LutProfileRemovalCase &test, bool profilesActive,
                                      const std::array<float, 2> &global) const
    {
        LutConstants lut{};
        lut.values[1] = {test.weights[0], test.weights[1], test.weights[2], test.weights[3]};
        const auto lutBuffer = CreateConstantBuffer(*device.device.Get(), lut);
        ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
        settings.vanillaGradeStrength = test.strength;
        settings.selectiveColorPlaneA.alpha = global[0];
        settings.selectiveColorPlaneB.alpha = global[1];
        settings.lutProfileLuminanceRemoval = {test.luminanceProfiles[0], test.luminanceProfiles[1],
                                               test.luminanceProfiles[2], test.luminanceProfiles[3]};
        settings.lutProfileColorRemoval = {test.colorProfiles[0], test.colorProfiles[1], test.colorProfiles[2],
                                           test.colorProfiles[3]};
        settings.lutProfilesActive = profilesActive ? 1U : 0U;
        const auto constants = CreateConstantBuffer(*device.device.Get(), settings);

        if (nativeMethod)
            Prepare(lutBuffer.Get());

        auto *additionalLook = nativeMethod ? nullptr : look.srv.Get();
        auto *lookSampler = nativeMethod ? nullptr : linear.Get();
        std::array<ID3D11ShaderResourceView *, 8> resources{
            scene.srv.Get(), nullptr,        nullptr,        nativeMethod ? prepared.srv.Get() : look.srv.Get(),
            additionalLook,  additionalLook, additionalLook, nullptr};
        std::array<ID3D11SamplerState *, 8> samplers{sceneSampler, nullptr,     nullptr,     lookSampler,
                                                     lookSampler,  lookSampler, lookSampler, nullptr};
        auto *target = output.rtv.Get();
        DrawFullscreen(*device.context.Get(),
                       {vertexShader, nativeMethod ? native.Get() : radial.Get(), lutBuffer.Get(), constants.Get(),
                        resources.data(), 8, samplers.data(), 8, &target, 1});
        device.context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                     "Map LUT profile grade removal");
        std::array<Float4, kWidth> result{};
        std::copy_n(static_cast<const Float4 *>(mapped.pData), kWidth, result.begin());
        device.context->Unmap(output.staging.Get(), 0);
        return result;
    }
};

void VerifyLutProfileGradeRemoval(Device &device, ID3D11VertexShader *vertexShader, ID3D11SamplerState *sceneSampler)
{
    constexpr std::array<Float4, kWidth> corpus{{{0, 0, 0, 0.125F},
                                                 {0.001F, 0.0005F, 0.00025F, 0.25F},
                                                 {0.18F, 0.04F, 0.01F, 0.375F},
                                                 {1, 0.25F, 0.0625F, 0.5F},
                                                 {4, 1, 0.25F, 0.625F},
                                                 {16, 4, 1, 0.75F},
                                                 {0.25F, 2, 0.125F, 0.875F},
                                                 {0.125F, 0.5F, 8, 1}}};
    D3D11_SUBRESOURCE_DATA sceneData{corpus.data(), sizeof(corpus), 0};
    const auto scene =
        CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &sceneData);
    const auto look = CreateNativeGrade(*device.device.Get(), 1);
    const auto linear = CreateClampLinearSampler(*device.device.Get());
    const auto radial = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kCreativeGradeLut);
    const auto native = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kNativeReferenceGrade);
    const auto prepare = CreateShader(*device.device.Get(), fo4hdr::generated::kNativeReferencePrepare);
    auto output = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);

    D3D11_TEXTURE3D_DESC preparedDescription{};
    preparedDescription.Width = 20;
    preparedDescription.Height = 16;
    preparedDescription.Depth = 16;
    preparedDescription.MipLevels = 1;
    preparedDescription.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    preparedDescription.Usage = D3D11_USAGE_DEFAULT;
    preparedDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Texture3D prepared;
    ComPtr<ID3D11UnorderedAccessView> preparedUav;
    CheckHresult(device.device->CreateTexture3D(&preparedDescription, nullptr, &prepared.texture),
                 "Create profile-blending prepared LUT");
    CheckHresult(device.device->CreateShaderResourceView(prepared.texture.Get(), nullptr, &prepared.srv),
                 "Create profile-blending prepared LUT SRV");
    CheckHresult(device.device->CreateUnorderedAccessView(prepared.texture.Get(), nullptr, &preparedUav),
                 "Create profile-blending prepared LUT UAV");

    const auto cases = LutProfileRemovalCases();

    const LutProfileRenderer renderer{device, vertexShader, sceneSampler, scene,  look,     linear,
                                      radial, native,       prepare,      output, prepared, preparedUav};
    const auto render = [&](bool nativeMethod, const LutProfileRemovalCase &test, bool active,
                            const std::array<float, 2> &global) {
        return renderer.Render(nativeMethod, test, active, global);
    };

    CheckLutProfileScenarios(render, corpus, cases);
}

}  // namespace fo4hdr::tests::gpu
