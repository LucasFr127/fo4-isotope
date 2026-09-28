// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "reference/NativeGradeReference.hpp"
#include "FO4Isotope/output/SelectiveColor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "FO4Isotope/tonemap/Aces2.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include <iostream>
#include <stdexcept>
#include "ShaderTestSuite.hpp"
#include <d3d11_1.h>
#include "reference/ImageControlReference.hpp"
#include "passes/NativeReferenceGradePass.hpp"

namespace fo4hdr::tests::gpu {

[[nodiscard]] Float4 ReferenceLookNode(UINT x, UINT y, UINT z, unsigned kind)
{
    const float r = static_cast<float>(x) / 15;
    const float g = static_cast<float>(y) / 15;
    const float b = static_cast<float>(z) / 15;
    if (kind == 0) return {r, g, b, 1};
    if (kind == 1) return {0, 0, 0, 1};
    if (kind == 2) return {0.04F, 0.08F, 0.12F, 1};
    if (kind == 3) {
        const float gray = 0.2125F * r + 0.7154F * g + 0.0721F * b;
        return {gray, gray, gray, 1};
    }
    return {0.03F + 0.6F * r + 0.1F * g * b,
        0.02F + 0.7F * g + 0.05F * r * b, 0.01F + 0.8F * b + 0.08F * r * g, 1};
}

constexpr std::array<Float4, kWidth> kReferenceGradeCorpus{{
    {0, 0, 0, 0}, {0.0001F, 0.0001F, 0.0001F, 0.25F},
    {0.18F, 0.18F, 0.18F, 0.5F}, {0.5F, 0.1F, 0.02F, 0.75F},
    {4, 0.5F, 0.125F, 1}, {0.1F, 8, 0.5F, 1},
    {64, 16, 4, 0.5F}, {65504, 100, 1, 1}}};

NativeGradeRgb ReferenceNeutralExpected(const std::array<Float4, 16 * 16 * 16>& nodes,
    UINT cell, double t)
{
    NativeGradeRgb expected{};
    for (UINT corner = 0; corner < 8; ++corner) {
        const UINT x = corner % 2, y = (corner / 2) % 2, z = corner / 4;
        const auto n = nodes[((cell + z) * 16 + cell + y) * 16 + cell + x];
        const double weight = (x ? t : 1 - t) * (y ? t : 1 - t) * (z ? t : 1 - t);
        expected[0] += weight * n.red;
        expected[1] += weight * n.green;
        expected[2] += weight * n.blue;
    }
    return expected;
}

void CheckNativeReferenceNeutralCoefficients(ID3D11DeviceContext& context,
    ID3D11Texture3D& readback, ID3D11Texture3D& prepared,
    const std::array<Float4, 16 * 16 * 16>& nodes)
{
    context.CopyResource(&readback, &prepared);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(&readback, 0, D3D11_MAP_READ, 0, &mapped), "Map prepared neutral coefficients");
    const auto nodeAt = [&](UINT x, UINT y) {
        return reinterpret_cast<const Float4*>(static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch)[x];
    };
    for (UINT cell = 0; cell < 15; ++cell)
        for (const double t : {0.0, 0.0001, 0.25, 0.5, 0.9999, 1.0}) {
            const auto expected = ReferenceNeutralExpected(nodes, cell, t);
            const auto a = nodeAt(16, cell), b = nodeAt(17, cell);
            const auto c = nodeAt(18, cell), d = nodeAt(19, cell);
            const NativeGradeRgb actual{a.red + t * (b.red + t * (c.red + t * d.red)),
                a.green + t * (b.green + t * (c.green + t * d.green)),
                a.blue + t * (b.blue + t * (c.blue + t * d.blue))};
            for (unsigned channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(std::abs(actual[channel] - expected[channel]) <= 2.0e-6);
        }
    const auto black = nodeAt(16, 15);
    FO4HDR_WARP_CHECK(std::abs(black.red - DecodeSrgb(nodes[0].red)) < 1.0e-6);
    FO4HDR_WARP_CHECK(std::abs(black.green - DecodeSrgb(nodes[0].green)) < 1.0e-6);
    FO4HDR_WARP_CHECK(std::abs(black.blue - DecodeSrgb(nodes[0].blue)) < 1.0e-6);
    context.Unmap(&readback, 0);
}

bool IsNativeReferenceContained(double value)
{
    return std::isfinite(value) && value >= 0 && value <= 65504;
}

template <class Render>
void VerifyNativeReferenceHostile(const Render& render, ID3D11PixelShader* shader,
    ToneMapConstants& settings)
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    const std::array<Float4, kWidth> hostile{{
        {nan, 1, 2, 0.25F}, {inf, 1, 2, 0.5F}, {-inf, -1, 0, 1},
        {0, nan, inf, 1}, {-1, 0.1F, 0.01F, 1},
        {65504, 65504, 65504, 1}, {0, 0, 0, 0}, {1, 2, 3, 1}}};
    settings.vanillaGradeStrength = 1;
    const auto contained = render(shader, hostile, settings, nullptr);
    for (std::size_t i = 0; i < kWidth; ++i) {
        for (const float value : {contained[i].red, contained[i].green, contained[i].blue})
            FO4HDR_WARP_CHECK(IsNativeReferenceContained(value));
        FO4HDR_WARP_CHECK(contained[i].alpha == hostile[i].alpha);
    }
    settings.vanillaGradeStrength = nan;
    const auto disabled = render(shader, kReferenceGradeCorpus, settings, nullptr);
    for (std::size_t i = 0; i < kWidth; ++i)
        FO4HDR_WARP_CHECK(disabled[i].red == kReferenceGradeCorpus[i].red &&
            disabled[i].green == kReferenceGradeCorpus[i].green && disabled[i].blue == kReferenceGradeCorpus[i].blue);
}

std::array<Float4, 16 * 16 * 16> MakeReferenceLookNodes(unsigned kind)
{
    std::array<Float4, 16 * 16 * 16> nodes{};
    for (UINT index = 0; index < nodes.size(); ++index)
        nodes[index] = ReferenceLookNode(index % 16, (index / 16) % 16, index / 256, kind);
    return nodes;
}

void QuantizeReferenceLookNodes(std::array<Float4, 16 * 16 * 16>& nodes,
    std::array<std::array<std::uint8_t, 4>, 16 * 16 * 16>& unormNodes)
{
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::array channels{nodes[i].red, nodes[i].green, nodes[i].blue, 1.0F};
        for (std::size_t c = 0; c < 4; ++c)
            unormNodes[i][c] = static_cast<std::uint8_t>(std::lround(channels[c] * 255));
        nodes[i] = {unormNodes[i][0] / 255.0F, unormNodes[i][1] / 255.0F,
            unormNodes[i][2] / 255.0F, 1};
    }
}

struct NativeReferenceLook
{
    std::array<Float4, 16 * 16 * 16> nodes{};
    Texture3D look;

    NativeReferenceLook(ID3D11Device& gpu, D3D11_TEXTURE3D_DESC desc, unsigned kind)
    {
        std::array<std::array<std::uint8_t, 4>, 16 * 16 * 16> unormNodes{};
        nodes = MakeReferenceLookNodes(kind);

        desc.Width = desc.Height = desc.Depth = 16;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D11_SUBRESOURCE_DATA data{nodes.data(), 16 * sizeof(Float4), 256 * sizeof(Float4)};
        if (kind == 4) {
            QuantizeReferenceLookNodes(nodes, unormNodes);
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            data = {unormNodes.data(), 16 * 4U, 256 * 4U};
        }
        CheckHresult(gpu.CreateTexture3D(&desc, &data, &look.texture), "Native reference fixture");
        CheckHresult(gpu.CreateShaderResourceView(look.texture.Get(), nullptr, &look.srv), "Native reference fixture SRV");
    }
};

struct NativeReferenceStrengthSample
{
    unsigned kind;
    float strength;
};

void CheckNativeReferenceKind(double value, double input, unsigned c,
    const NativeReferenceStrengthSample& sample)
{
    if (sample.kind == 0) FO4HDR_WARP_CHECK(std::abs(value - input) <= 0.0001 + 0.0001 * input);
    if (sample.strength != 1) return;
    if (sample.kind == 1) FO4HDR_WARP_CHECK(value == 0);
    if (sample.kind == 2)
        FO4HDR_WARP_CHECK(std::abs(value - DecodeSrgb(0.04F * (c + 1))) < 1.0e-4);
}

void CheckNativeReferenceStrength(const std::array<Float4, kWidth>& actual,
    const std::array<Float4, kWidth>& full, const NativeReferenceStrengthSample& sample)
{
    for (std::size_t i = 0; i < kWidth; ++i) {
        const NativeGradeRgb input{kReferenceGradeCorpus[i].red, kReferenceGradeCorpus[i].green, kReferenceGradeCorpus[i].blue};
        const NativeGradeRgb end{full[i].red, full[i].green, full[i].blue};
        const NativeGradeRgb value{actual[i].red, actual[i].green, actual[i].blue};
        for (unsigned c = 0; c < 3; ++c) {
            FO4HDR_WARP_CHECK(IsNativeReferenceContained(value[c]));
            FO4HDR_WARP_CHECK(std::abs(value[c] - ((1 - sample.strength) * input[c] + sample.strength * end[c])) <= 2.0e-6 * std::max(1.0, input[c]));
            if (sample.strength == 0) FO4HDR_WARP_CHECK(value[c] == input[c]);
            CheckNativeReferenceKind(value[c], input[c], c, sample);
        }
        if (sample.kind == 3 && sample.strength == 1)
            FO4HDR_WARP_CHECK(std::max({value[0], value[1], value[2]}) - std::min({value[0], value[1], value[2]}) < 1.0e-4 * std::max(1.0, input[0]));
        FO4HDR_WARP_CHECK(actual[i].alpha == kReferenceGradeCorpus[i].alpha);
    }
}

template <class Render>
std::array<Float4, kWidth> VerifyNativeReferenceStrength(const Render& render,
    ID3D11PixelShader* shader, ToneMapConstants& settings, unsigned kind)
{
    settings.vanillaGradeStrength = 1;
    const auto full = render(shader, kReferenceGradeCorpus, settings, nullptr);
    for (const float strength : {0.0F, 0.5F, 1.0F}) {
        settings.vanillaGradeStrength = strength;
        const auto actual = render(shader, kReferenceGradeCorpus, settings, nullptr);
        CheckNativeReferenceStrength(actual, full, {kind, strength});
    }
    return full;
}

struct NativeReferenceOperatorTest
{
    ID3D11PixelShader* shader;
    ID3D11PixelShader* radial;
    const std::array<Float4, kWidth>& full;
};

template <class Render>
void VerifyNativeReferenceOperators(Device& device, const Render& render,
    ToneMapConstants& settings, const NativeReferenceOperatorTest& test)
{
    for (const auto mapper : {TestedToneMapper::aces2Lmt, TestedToneMapper::agX, TestedToneMapper::gt7}) {
        const auto algorithm = ProductionAlgorithm(mapper);
        const auto op = CreateOperatorLut(*device.device.Get(), algorithm, 1000);
        settings.productionMode = ProductionMode(mapper);
        settings.contrastSlope = 1.1F;
        settings.shadowsEv = -0.5F;
        settings.blacksStrength = -0.25F;
        settings.saturation = 0.8F;
        fo4hdr::SelectiveColorSettings selective{};
        selective.enabled = true;
        selective.regions[0] = {8, 125, 20};
        fo4hdr::SelectiveColorPrepared sc{};
        FO4HDR_WARP_CHECK(fo4hdr::PrepareSelectiveColor(selective, sc));
        settings.selectiveColorActive = sc.effectiveActive ? 1U : 0U;
        for (std::size_t i = 0; i < sc.regions.size(); ++i)
            settings.selectiveColorRegions[i] = {static_cast<float>(sc.regions[i][0]), static_cast<float>(sc.regions[i][1]), static_cast<float>(sc.regions[i][2]), 0};
        settings.selectiveColorPlaneA = {static_cast<float>(sc.planeA[0]), static_cast<float>(sc.planeA[1]), static_cast<float>(sc.planeA[2]), 0};
        settings.selectiveColorPlaneB = {static_cast<float>(sc.planeB[0]), static_cast<float>(sc.planeB[1]), static_cast<float>(sc.planeB[2]), 0};
        settings.vanillaGradeStrength = 1;
        const auto actual = render(test.shader, kReferenceGradeCorpus, settings, op.srv.Get());
        settings.vanillaGradeStrength = 0;
        const auto expected = render(test.radial, test.full, settings, op.srv.Get());
        for (std::size_t i = 0; i < kWidth; ++i) {
            FO4HDR_WARP_CHECK(std::abs(actual[i].red - expected[i].red) < 2.0e-5F);
            FO4HDR_WARP_CHECK(std::abs(actual[i].green - expected[i].green) < 2.0e-5F);
            FO4HDR_WARP_CHECK(std::abs(actual[i].blue - expected[i].blue) < 2.0e-5F);
            FO4HDR_WARP_CHECK(actual[i].alpha == expected[i].alpha);
        }
    }
}

struct NativeReferencePreparation
{
    ID3D11ComputeShader* shader;
    ID3D11UnorderedAccessView* output;
};

void PrepareNativeReferenceLook(Device& device, ID3D11ShaderResourceView* look,
    unsigned kind, const NativeReferencePreparation& preparation)
{
    LutConstants weights{};
    constexpr std::array<Float4, 5> mixtures{{
        {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1},
        {0.125F, 0.25F, 0.125F, 0.5F}}};
    weights.values[1] = mixtures[kind];
    const auto constants = CreateConstantBuffer(*device.device.Get(), weights);
    auto* cb = constants.Get();
    auto* uav = preparation.output;
    std::array<ID3D11ShaderResourceView*, 4> sources{};
    if (kind < 4) sources[kind] = look;
    else sources.fill(look);
    device.context->CSSetShader(preparation.shader, nullptr, 0);
    device.context->CSSetConstantBuffers(2, 1, &cb);
    device.context->CSSetShaderResources(0, 4, sources.data());
    device.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    device.context->Dispatch(2, 2, 2);
    uav = nullptr;
    cb = nullptr;
    sources.fill(nullptr);
    device.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    device.context->CSSetShaderResources(0, 4, sources.data());
    device.context->CSSetConstantBuffers(2, 1, &cb);
    device.context->CSSetShader(nullptr, nullptr, 0);
}

struct NativeReferencePrepared
{
    D3D11_TEXTURE3D_DESC desc{};
    Texture3D prepared;
    ComPtr<ID3D11UnorderedAccessView> preparedUav;
    ComPtr<ID3D11Texture3D> readback;

    explicit NativeReferencePrepared(ID3D11Device& gpu)
    {
        desc.Width = 20;
        desc.Height = desc.Depth = 16;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        CheckHresult(gpu.CreateTexture3D(&desc, nullptr, &prepared.texture), "Native reference prepared texture");
        CheckHresult(gpu.CreateShaderResourceView(prepared.texture.Get(), nullptr, &prepared.srv), "Native reference prepared SRV");
        CheckHresult(gpu.CreateUnorderedAccessView(prepared.texture.Get(), nullptr, &preparedUav), "Native reference prepared UAV");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHresult(gpu.CreateTexture3D(&desc, nullptr, &readback), "Native reference prepared staging");
        }
};

std::array<Float4, kWidth> ReadNativeReferenceOutput(ID3D11DeviceContext& context,
    Texture& output)
{
    context.CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map Native Reference grade");
    std::array<Float4, kWidth> values{};
    std::copy_n(static_cast<const Float4*>(mapped.pData), kWidth, values.begin());
    context.Unmap(output.staging.Get(), 0);
    return values;
}

void VerifyNativeReferenceGrade(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sceneSampler)
{
    const auto prepare = CreateShader(*device.device.Get(), fo4hdr::generated::kNativeReferencePrepare);
    const auto shader = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kNativeReferenceGrade);
    const auto radial = CreatePixelShader(*device.device.Get(), fo4hdr::generated::kCreativeGradeLut);
    const auto linear = CreateClampLinearSampler(*device.device.Get());
    NativeReferencePrepared preparation(*device.device.Get());
    auto& prepared = preparation.prepared;
    auto& preparedUav = preparation.preparedUav;
    auto& readback = preparation.readback;
    const auto& desc = preparation.desc;
    auto output = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    const auto render = [&](ID3D11PixelShader* ps, const std::array<Float4, kWidth>& pixels,
                            ToneMapConstants settings, ID3D11ShaderResourceView* op) {
        D3D11_SUBRESOURCE_DATA data{pixels.data(), sizeof(pixels), 0};
        auto input = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, &data);
        const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
        std::array<ID3D11ShaderResourceView*, 8> resources{input.srv.Get(), nullptr,
            nullptr, prepared.srv.Get(), nullptr, nullptr, nullptr, op};
        std::array<ID3D11SamplerState*, 8> samplers{sceneSampler, nullptr, nullptr,
            linear.Get(), nullptr, nullptr, nullptr, linear.Get()};
        auto* target = output.rtv.Get();
        DrawFullscreen(*device.context.Get(), { vertexShader, ps, nullptr, constants.Get(),
            resources.data(), 8, samplers.data(), 8, &target, 1});
        return ReadNativeReferenceOutput(*device.context.Get(), output);
    };
    ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
    for (unsigned kind = 0; kind < 5; ++kind) {
        NativeReferenceLook fixture(*device.device.Get(), desc, kind);
        const auto& look = fixture.look;
        const auto& nodes = fixture.nodes;
        PrepareNativeReferenceLook(device, look.srv.Get(), kind,
            {prepare.Get(), preparedUav.Get()});

        CheckNativeReferenceNeutralCoefficients(*device.context.Get(),
            *readback.Get(), *prepared.texture.Get(), nodes);

        if (kind == 0) VerifyNativeReferenceHostile(render, shader.Get(), settings);

        const auto full = VerifyNativeReferenceStrength(render, shader.Get(), settings, kind);

        if (kind != 4) continue;
        VerifyNativeReferenceOperators(device, render, settings,
            {shader.Get(), radial.Get(), full});
    }
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

struct NativeGradeCase
{
    TestedToneMapper mapper;
    float peak;
    std::size_t corpusIndex;
    std::size_t mix;
    float strength;
    std::size_t pixel;
};

struct NativeGradeCheck
{
    NativeGradeRgb source;
    NativeGradeRgb reference;
    NativeGradeRgb sampledReference;
    NativeGradeRgb actual;
};

struct NativeGradeNitsCheck
{
    const NativeGradeRgb& reference;
    NativeGradeRgb& nits;
    const NativeGradeRgb& oracle;
    const std::array<std::uint16_t,4>& half;
};

struct NativeGradeMeasurements
{
    double maxGradeError{}, maxGradeArithmeticError{};
    double maxNitsError{}, maxDarkNitsError{};
    double maxNeutralSpread{}, maxIdentityRatioChange{};
    unsigned maxSourceR10Distance{};
    std::size_t sampleCount{};
    unsigned gradeFailureDiagnostics{};

    void CheckIdentityRatio(const NativeGradeCheck& colors, const NativeGradeCase& test)
    {
        if (test.mix != 0 || test.strength != 1) return;
        if (!(colors.source[0] > 0 && colors.source[1] > 0)) return;
        maxIdentityRatioChange = std::max(maxIdentityRatioChange,
            std::abs(colors.actual[0] / colors.actual[1] - colors.source[0] / colors.source[1]));
    }
    void CheckGrade(const NativeGradeCheck& colors, const NativeGradeCase& test)
    {
        for (std::size_t c = 0; c < 3; ++c) {
            const double error = std::abs(colors.actual[c]-colors.reference[c]);
            maxGradeError = std::max(maxGradeError,error);
            const double arithmeticError = std::abs(colors.sampledReference[c]-colors.actual[c]);
            maxGradeArithmeticError = std::max(maxGradeArithmeticError,arithmeticError);
            // Float arithmetic/sampling parity, not an image-quality budget.
            if (arithmeticError > 3.0e-5 * std::max(1.0,colors.reference[c]) && gradeFailureDiagnostics++ < 12) {
                std::cerr << "Native grading grade mismatch corpus=" << test.corpusIndex << " mix=" << test.mix
                    << " strength=" << test.strength << " pixel=" << test.pixel << " channel=" << c
                    << " input=" << colors.source[0] << ',' << colors.source[1] << ',' << colors.source[2]
                    << " actual=" << colors.actual[c] << " ideal=" << colors.reference[c]
                    << " sampled_reference=" << colors.sampledReference[c] << '\n';
            }
            FO4HDR_WARP_CHECK(arithmeticError <= 3.0e-5 * std::max(1.0,colors.reference[c]));
            FO4HDR_WARP_CHECK(std::isfinite(colors.actual[c]) && colors.actual[c] >= 0);
            if (test.strength == 0) FO4HDR_WARP_CHECK(colors.actual[c] == colors.source[c]);
        }
        CheckIdentityRatio(colors, test);
    }
    void CheckNits(const NativeGradeNitsCheck& colors,
        const NativeGradeCase& test)
    {
        for (std::size_t c = 0; c < 3; ++c) {
            colors.nits[c] = Binary16ToFloat(colors.half[c]);
            const double error = std::abs(colors.nits[c]-colors.oracle[c]);
            maxNitsError = std::max(maxNitsError,error);
            if (*std::max_element(colors.reference.begin(),colors.reference.end()) <= 0.01)
                maxDarkNitsError = std::max(maxDarkNitsError,error);
            // 0.5%-peak LUT budget, plus both FP16 stores.
            const double rounding = Binary16LargestAdjacentStep(static_cast<float>(colors.oracle[c]/203)) * 203 +
                Binary16LargestAdjacentStep(static_cast<float>(colors.oracle[c]));
            FO4HDR_WARP_CHECK(std::isfinite(colors.nits[c]) && colors.nits[c] >= 0);
            FO4HDR_WARP_CHECK(error <= std::max(1.0,test.peak*0.005) + rounding);
        }
        if (test.corpusIndex == 0 && test.mix == 0)
            maxNeutralSpread = std::max(maxNeutralSpread,
                *std::max_element(colors.nits.begin(),colors.nits.end()) -
                *std::min_element(colors.nits.begin(),colors.nits.end()));
    }
    void CheckOutput(std::uint32_t packed, const NativeGradeRgb& nits,
        const NativeGradeRgb& oracle)
    {
        const auto pq = fo4hdr::EncodeSt2084Pq({nits[0],nits[1],nits[2]});
        const auto sourcePq = fo4hdr::EncodeSt2084Pq({oracle[0],oracle[1],oracle[2]});
        const NativeGradeRgb encoded{pq.red,pq.green,pq.blue}, sourceEncoded{sourcePq.red,sourcePq.green,sourcePq.blue};
        for (unsigned c = 0; c < 3; ++c) {
            const auto code = static_cast<std::uint16_t>((packed >> (10*c)) & 1023);
            FO4HDR_WARP_CHECK(CodeDistance(code,fo4hdr::QuantizeR10Unorm(encoded[c])) <= 1U);
            maxSourceR10Distance = std::max(maxSourceR10Distance,
                static_cast<unsigned>(CodeDistance(code,fo4hdr::QuantizeR10Unorm(sourceEncoded[c]))));
        }
        FO4HDR_WARP_CHECK((packed >> 30) == 3);
    }
    void Report(TestedToneMapper mapper, float peak) const
    {
        std::cout << "Native grading characterization mode=" << ProductionMode(mapper) << " peak=" << peak
            << " samples=" << sampleCount << " max_grade_abs=" << maxGradeError
            << " sampled_grade_arithmetic_abs=" << maxGradeArithmeticError
            << " max_source_nits_abs=" << maxNitsError << " dark_source_nits_abs=" << maxDarkNitsError
            << " max_source_r10_distance=" << maxSourceR10Distance
            << " neutral_nits_spread=" << maxNeutralSpread
            << " identity_grade_red_green_ratio_change=" << maxIdentityRatioChange << '\n';
    }
};

std::array<NativeGradeRgb,kWidth> ReadNativeGradeRgb(ID3D11DeviceContext& context,
    Texture& texture)
{
    context.CopyResource(texture.staging.Get(),texture.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(texture.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Native grading sample probe readback");
    const auto* pixels = static_cast<const Float4*>(mapped.pData);
    std::array<NativeGradeRgb,kWidth> result{};
    for (std::size_t i = 0; i < kWidth; ++i)
        result[i] = {pixels[i].red,pixels[i].green,pixels[i].blue};
    context.Unmap(texture.staging.Get(),0);
    return result;
}

fo4hdr::Aces2TransformHandle CreateNativeGradeOracle(TestedToneMapper mapper, float peak)
{
    // Reuse the immutable source-oracle tables for this peak.
    auto aces = mapper == TestedToneMapper::aces2Lmt ?
        fo4hdr::CreateAces2Transform(peak) : fo4hdr::Aces2TransformHandle{};
    if (mapper == TestedToneMapper::aces2Lmt && !aces)
        throw std::runtime_error("Native grading ACES source oracle creation failed");
    return aces;
}

bool NativeGradeAnchorMatches(const NativeGradeCase& test, float strength,
    std::size_t corpusIndex, std::size_t pixel)
{
    return test.strength == strength && test.corpusIndex == corpusIndex && test.pixel == pixel;
}

void PrintNativeGradeAnchors(const NativeGradeRgb& reference,
    const NativeGradeRgb& nits, std::uint32_t packed, const NativeGradeCase& test)
{
    if (test.mix != 0) return;
    if (NativeGradeAnchorMatches(test, 1, 1, 6))
        std::cout << "Native grading anchor mode=" << ProductionMode(test.mapper) << " peak=" << test.peak
            << " input=16,4,1 grade=" << reference[0] << ',' << reference[1] << ',' << reference[2]
            << " nits=" << nits[0] << ',' << nits[1] << ',' << nits[2]
            << " r10=" << (packed&1023) << ',' << ((packed>>10)&1023) << ',' << ((packed>>20)&1023) << '\n';
    if (NativeGradeAnchorMatches(test, 0, 0, 0))
        std::cout << "Native grading artistic-off black mode=" << ProductionMode(test.mapper) << " peak=" << test.peak
            << " nits=" << nits[0] << ',' << nits[1] << ',' << nits[2]
            << " r10=" << (packed&1023) << ',' << ((packed>>10)&1023) << ',' << ((packed>>20)&1023) << '\n';
}

struct NativeGradeComposed
{
    std::array<NativeGradeRgb,kWidth> nits;
    std::array<NativeGradeRgb,kWidth> oracle;
};

struct NativeGradeReadback
{
    Device& device;
    const std::array<std::array<std::uint16_t,4>,kWidth>& pixels;
    const std::array<NativeGradeRgb,kWidth>& sampled;
    const std::array<float,4>& w;
    NativeGradeMeasurements& stats;
    NativeGradeCase test;
    std::array<NativeGradeRgb,kWidth> ReadGrade(Texture& texture)
    {
        device.context->CopyResource(texture.staging.Get(),texture.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(texture.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Native grading grade readback");
        const auto* values = static_cast<const Float4*>(mapped.pData);
        std::array<NativeGradeRgb,kWidth> references{};
        for (std::size_t i = 0; i < kWidth; ++i) {
            const NativeGradeRgb source{Binary16ToFloat(pixels[i][0]),Binary16ToFloat(pixels[i][1]),Binary16ToFloat(pixels[i][2])};
            references[i] = FrozenNativeGrade(source,w,test.strength);
            const auto idealCodes = SampleSyntheticNativeGrade(source, w);
            // The affine table slopes are at most one and these weights
            // sum to one. Allow two 8-bit hardware fraction steps plus
            // FP32 roundoff, independently of the shipping coordinates.
            for (std::size_t c = 0; c < 3; ++c)
                FO4HDR_WARP_CHECK(std::abs(sampled[i][c] - idealCodes[c]) <=
                    2.0 / (15.0 * 256.0) + 2.0e-6);
            // Observe sampler/guide precision independently of the
            // production grade result. The double-precision CPU oracle
            // then evaluates gamma decode and inverse radial scaling; no grade
            // output is fed back as its own expected value.
            const auto sampledReference = DecodeAndTransferNativeGrade(source,sampled[i],test.strength);
            FO4HDR_WARP_CHECK(values[i].alpha == Binary16ToFloat(pixels[i][3]));
            const NativeGradeRgb actual{values[i].red,values[i].green,values[i].blue};
            stats.CheckGrade({source, references[i], sampledReference, actual},
                {test.mapper, test.peak, test.corpusIndex, test.mix, test.strength, i});
        }
        device.context->Unmap(texture.staging.Get(),0);
        return references;
    }

    void CheckAlpha(Texture& texture)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        device.context->CopyResource(texture.staging.Get(),texture.texture.Get());
        CheckHresult(device.context->Map(texture.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Native grading alpha readback");
        const auto* transported = static_cast<const std::array<std::uint16_t,4>*>(mapped.pData);
        for (std::size_t i = 0; i < kWidth; ++i)
            FO4HDR_WARP_CHECK(transported[i][3] == pixels[i][3]);
        device.context->Unmap(texture.staging.Get(),0);
    }

    NativeGradeComposed ReadComposition(Texture& texture,
        const std::array<NativeGradeRgb,kWidth>& references,
        const fo4hdr::Aces2TransformHandle& aces)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        device.context->CopyResource(texture.staging.Get(),texture.texture.Get());
        CheckHresult(device.context->Map(texture.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Native grading composition readback");
        const auto* half = static_cast<const std::array<std::uint16_t,4>*>(mapped.pData);
        std::array<NativeGradeRgb,kWidth> nits{}, oracle{};
        for (std::size_t i = 0; i < kWidth; ++i) {
            const auto& r = references[i];
            const auto ref = aces ?
                fo4hdr::MapAces2LmtToBt2020Nits(*aces,{r[0],r[1],r[2]}) :
                MapProductionOperator(test.mapper,{r[0],r[1],r[2]},test.peak);
            oracle[i] = {ref.red,ref.green,ref.blue};
            stats.CheckNits({r, nits[i], oracle[i], half[i]},
                {test.mapper, test.peak, test.corpusIndex, test.mix, test.strength, i});
        }
        device.context->Unmap(texture.staging.Get(),0);
        return {nits, oracle};
    }

    void CheckOutput(Texture& texture, const NativeGradeComposed& composed,
        const std::array<NativeGradeRgb,kWidth>& references)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        device.context->CopyResource(texture.staging.Get(),texture.texture.Get());
        CheckHresult(device.context->Map(texture.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Native grading R10 readback");
        const auto* codes = static_cast<const std::uint32_t*>(mapped.pData);
        for (std::size_t i = 0; i < kWidth; ++i) {
            stats.CheckOutput(codes[i], composed.nits[i], composed.oracle[i]);
            PrintNativeGradeAnchors(references[i], composed.nits[i], codes[i],
                {test.mapper, test.peak, test.corpusIndex, test.mix, test.strength, i});
            ++stats.sampleCount;
        }
        device.context->Unmap(texture.staging.Get(),0);
    }
};

constexpr std::array<std::array<Float4, kWidth>, 3> kNativeGradeCorpora{{
    {{{0,0,0,0}, {0.00001F,0.00001F,0.00001F,0.25F},
      {0.0001F,0.0001F,0.0001F,0.5F}, {0.001F,0.001F,0.001F,0.75F},
      {0.01F,0.01F,0.01F,1}, {0.18F,0.18F,0.18F,1}, {1,1,1,1}, {16,16,16,1}}},
    {{{0.0001F,0.000025F,0.00000625F,1}, {0.001F,0.00025F,0.0000625F,1},
      {0.01F,0.0025F,0.000625F,1}, {0.1F,0.025F,0.00625F,1},
      {1,0.25F,0.0625F,1}, {4,1,0.25F,1}, {16,4,1,1}, {64,16,4,1}}},
    {{{0.00001F,0.00004F,0.00016F,1}, {0.001F,0.004F,0.016F,1},
      {0.125F,0.5F,2,1}, {0.5F,2,8,1}, {2,8,32,1},
      {2.03125F,0.6875F,0.53125F,1}, {8,2,4,1}, {2,16,4,1}}}
}};
constexpr std::array<std::array<float, 4>, 9> kNativeGradeMixtures{{
    {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1},
    {0.25F,0.25F,0.25F,0.25F}, {0,0.75F,0.25F,0},
    {0,0.5F,0.5F,0}, {0,0.25F,0.75F,0}, {0.1F,0.2F,0.3F,0.4F}}};

struct NativeGradeShaders
{
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11PixelShader> gradeShader, sampleShader;
    ComPtr<ID3D11ComputeShader> composeShader, pqShader;

    explicit NativeGradeShaders(ID3D11Device& gpu)
    {
        linear = CreateClampLinearSampler(gpu);
        gradeShader = CreatePixelShader(gpu, fo4hdr::generated::kCreativeGradeLut);
        sampleShader = CreatePixelShader(gpu,fo4hdr::generated::test::kNativeGradeSamplingPs);
        composeShader = CreateShader(gpu, fo4hdr::generated::test::kLayerEvaluationCs);
        pqShader = CreateShader(gpu, fo4hdr::generated::test::kTransferEvaluationCs);
    }
};

Texture CreateNativeGradeTransparentUi(ID3D11Device& gpu)
{
    const auto transparent = MakeFp16Pixels(UniformPixels({}));
    D3D11_SUBRESOURCE_DATA transparentData{transparent.data(), sizeof(transparent), 0};
    return CreateTexture(gpu, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &transparentData);
}

ComPtr<ID3D11Buffer> CreateNativeGradeWeights(ID3D11Device& gpu,
    const std::array<float,4>& w)
{
    LutConstants lv{};
    lv.values[1] = {w[0],w[1],w[2],w[3]};
    return CreateConstantBuffer(gpu, lv);
}

void VerifyNativeGradeColorChain(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sceneSampler)
{
    std::array<Texture3D, 4> grades;
    for (unsigned i = 0; i < 4; ++i) grades[i] = CreateNativeGrade(*device.device.Get(), i);
    const NativeGradeShaders shaders(*device.device.Get());
    auto gradeOnly = CreateTexture(*device.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto sampledGrade = CreateTexture(*device.device.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_RENDER_TARGET);
    auto sampledGuide = CreateTexture(*device.device.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_RENDER_TARGET);
    auto transport = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto composition = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    auto output = CreateTexture(*device.device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_UNORDERED_ACCESS);
    auto ui = CreateNativeGradeTransparentUi(*device.device.Get());
    const auto pqConstants = CreateConstantBuffer(*device.device.Get(), OutputConstants{kWidth,kHeight,1,0});
    std::array<ID3D11SamplerState*, 8> samplers{sceneSampler,nullptr,nullptr,
        shaders.linear.Get(),shaders.linear.Get(),shaders.linear.Get(),shaders.linear.Get(),shaders.linear.Get()};
    for (const auto mapper : {TestedToneMapper::aces2Lmt, TestedToneMapper::agX, TestedToneMapper::gt7})
    for (const float peak : {400.0F, 1000.0F, 2000.0F}) {
        const auto algorithm = ProductionAlgorithm(mapper);
        const auto operatorLut = CreateOperatorLut(*device.device.Get(), algorithm, peak);
        const auto aces = CreateNativeGradeOracle(mapper, peak);
        const auto composeConstants = CreateConstantBuffer(*device.device.Get(),
            CompositionConstants{kWidth,kHeight,3,203,203,peak,0,0});
        NativeGradeMeasurements stats;
        for (std::size_t corpusIndex = 0; corpusIndex < kNativeGradeCorpora.size(); ++corpusIndex) {
            const auto pixels = MakeFp16Pixels(kNativeGradeCorpora[corpusIndex]);
            D3D11_SUBRESOURCE_DATA data{pixels.data(), sizeof(pixels), 0};
            auto scene = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data);
            std::array<ID3D11ShaderResourceView*, 8> resources{scene.srv.Get(),nullptr,nullptr,
                grades[0].srv.Get(),grades[1].srv.Get(),grades[2].srv.Get(),grades[3].srv.Get(),operatorLut.srv.Get()};
            for (std::size_t mix = 0; mix < kNativeGradeMixtures.size(); ++mix)
            for (const float strength : {0.0F, 0.5F, 1.0F}) {
                const auto& w = kNativeGradeMixtures[mix];
                const auto lc = CreateNativeGradeWeights(*device.device.Get(), w);
                ToneMapConstants settings{0,peak,203,203,1,203,
                    static_cast<float>(fo4hdr::OperatorLutInputShaperMinimum(algorithm,peak)),
                    static_cast<float>(fo4hdr::OperatorLutInputShaperMaximum(algorithm,peak))};
                settings.vanillaGradeStrength = strength;
                auto constants = CreateConstantBuffer(*device.device.Get(), settings);
                std::array<ID3D11RenderTargetView*,2> probeTargets{sampledGrade.rtv.Get(),sampledGuide.rtv.Get()};
                DrawFullscreen(*device.context.Get(), {vertexShader,shaders.sampleShader.Get(),lc.Get(),constants.Get(),
                    resources.data(),8,samplers.data(),8,probeTargets.data(),2});
                const auto sampled = ReadNativeGradeRgb(*device.context.Get(), sampledGrade);
                ID3D11RenderTargetView* target = gradeOnly.rtv.Get();
                DrawFullscreen(*device.context.Get(), {vertexShader,shaders.gradeShader.Get(),lc.Get(),constants.Get(),
                    resources.data(),8,samplers.data(),8,&target,1});
                NativeGradeReadback readback{device, pixels, sampled, w, stats,
                    {mapper, peak, corpusIndex, mix, strength, 0}};
                const auto references = readback.ReadGrade(gradeOnly);
                settings.productionMode = ProductionMode(mapper);
                constants = CreateConstantBuffer(*device.device.Get(),settings);
                target = transport.rtv.Get();
                DrawFullscreen(*device.context.Get(), {vertexShader,shaders.gradeShader.Get(),lc.Get(),constants.Get(),
                    resources.data(),8,samplers.data(),8,&target,1});
                readback.CheckAlpha(transport);
                std::array<ID3D11ShaderResourceView*,4> inputs{transport.srv.Get(),nullptr,nullptr,ui.srv.Get()};
                Dispatch(*device.context.Get(), {shaders.composeShader.Get(),composeConstants.Get(),inputs.data(),4,composition.uav.Get()});
                const auto composed = readback.ReadComposition(composition, references, aces);
                auto* pqInput = composition.srv.Get();
                Dispatch(*device.context.Get(), {shaders.pqShader.Get(),pqConstants.Get(),&pqInput,1,output.uav.Get()});
                readback.CheckOutput(output, composed, references);
            }
        }
        stats.Report(mapper, peak);
    }
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

struct NativeGradeDraw
{
    Device& device;
    NativeReferenceResources& prepared;
    ID3D11VertexShader* vertexShader;
    ID3D11SamplerState* sampler;
    Texture& output;
    const Texture& scene;
    std::array<Float4, kWidth> Run(float strength, float luminanceRemoval, float colorRemoval)
    {
        ToneMapConstants settings{0, 1000, 203, 203, 1, 203, 0, 0};
        settings.vanillaGradeStrength = strength;
        settings.selectiveColorPlaneA.alpha = luminanceRemoval;
        settings.selectiveColorPlaneB.alpha = colorRemoval;
        const auto constants = CreateConstantBuffer(*device.device.Get(), settings);
        std::array<ID3D11ShaderResourceView*, 8> resources{
            scene.srv.Get(), nullptr, nullptr, prepared.preblendSrv.Get(),
            nullptr, nullptr, nullptr, nullptr};
        std::array<ID3D11SamplerState*, 8> samplers{
            sampler, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, sampler};
        auto* target = output.rtv.Get();
        DrawFullscreen(*device.context.Get(), { vertexShader,
            prepared.shader.Get(), nullptr, constants.Get(), resources.data(),
            8, samplers.data(), 8, &target, 1});
        device.context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(output.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped), "Map native grade removal");
        std::array<Float4, kWidth> result{};
        std::copy_n(static_cast<const Float4*>(mapped.pData), kWidth,
            result.begin());
        device.context->Unmap(output.staging.Get(), 0);
        return result;
    }
};

struct NativeRemovalCase
{
    std::array<float, 2> removal;
    std::size_t index;
};
void CheckNativeRemovalChroma(const NativeGradeRgb& hdr, const auto& channels,
    const auto& expected, NativeRemovalCase test)
{
    const auto& [removal, index] = test;
    constexpr NativeGradeRgb luma{
        0.212500006, 0.715399981, 0.0720999986};
    const auto dot = [&](const auto& value) {
        return value[0] * luma[0] + value[1] * luma[1] +
            value[2] * luma[2];
    };
    if (removal[1] == 1 && dot(hdr) > 0)
        for (std::size_t channel = 1; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(std::abs(
                channels[0] * hdr[channel] -
                channels[channel] * hdr[0]) <=
                3.0e-5 * std::max(1.0, expected[0]));
    if (removal[0] == 1 && index < 5)
        FO4HDR_WARP_CHECK(std::abs(dot(channels) - dot(hdr)) <=
            3.0e-5 * std::max(1.0, dot(hdr)));
}
struct NativeRemovalSample
{
    const Float4& original;
    const Float4& baseline;
    const Float4& actual;
    std::array<float, 2> removal;
    std::size_t index;
};
void CheckNativeRemovalPixel(NativeRemovalSample sample)
{
    const auto& removal = sample.removal;
    const auto index = sample.index;
    const NativeGradeRgb hdr{sample.original.red,
        sample.original.green, sample.original.blue};
    const NativeGradeRgb graded{sample.baseline.red,
        sample.baseline.green, sample.baseline.blue};
    const auto expected = ApplyVanillaGradeRemovalOracle(
        hdr, graded, removal[0], removal[1]);
    const std::array channels{sample.actual.red,
        sample.actual.green, sample.actual.blue};
    const std::array baselineChannels{sample.baseline.red,
        sample.baseline.green, sample.baseline.blue};
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const auto tolerance = 3.0e-5 *
            std::max(1.0, expected[channel]);
        FO4HDR_WARP_CHECK(std::isfinite(channels[channel]));
        FO4HDR_WARP_CHECK(channels[channel] >= 0.0F &&
            channels[channel] <= 65504.0F);
        FO4HDR_WARP_CHECK(std::abs(channels[channel] -
            expected[channel]) <= tolerance);
        if (removal[0] == 0 && removal[1] == 0)
            FO4HDR_WARP_CHECK(channels[channel] ==
                baselineChannels[channel]);
        if (removal[0] == 1 && removal[1] == 1)
            FO4HDR_WARP_CHECK(channels[channel] == hdr[channel]);
    }
    FO4HDR_WARP_CHECK(sample.actual.alpha == sample.original.alpha);
    CheckNativeRemovalChroma(hdr, channels, expected, {removal, index});
}

struct NativeGradeCorners
{
    const std::array<Float4, kWidth>& baseline;
    const std::array<Float4, kWidth>& neutralized;
    const std::array<Float4, kWidth>& restored;
};
void CheckNativeGradeCorners(NativeGradeCorners results,
    const std::array<Float4, kWidth>& corpus, bool gradedLook)
{
    const auto& [baseline, neutralized, restored] = results;
        if (gradedLook)
        {
            FO4HDR_WARP_CHECK(baseline[0].red > baseline[0].green);
            FO4HDR_WARP_CHECK(neutralized[0].red > 0);
            FO4HDR_WARP_CHECK(std::abs(neutralized[0].red -
                neutralized[0].green) <= 1.0e-6F);
            FO4HDR_WARP_CHECK(std::abs(neutralized[0].green -
                neutralized[0].blue) <= 1.0e-6F);
        }
        else
        {
            FO4HDR_WARP_CHECK(baseline[1].red == 0 &&
                baseline[1].green == 0 && baseline[1].blue == 0);
            FO4HDR_WARP_CHECK(std::abs(restored[1].red - corpus[1].red) <=
                1.0e-7F);
            FO4HDR_WARP_CHECK(std::abs(restored[1].green - corpus[1].green) <=
                1.0e-7F);
            FO4HDR_WARP_CHECK(std::abs(restored[1].blue - corpus[1].blue) <=
                1.0e-7F);
        }
}

void PrepareNativeGrades(Device& device, std::array<Texture3D, 4>& grades, NativeReferenceInputs& inputs)
{
    for (unsigned grade = 0; grade < grades.size(); ++grade)
        grades[grade] = CreateNativeGrade(*device.device.Get(), grade);
    for (std::size_t grade = 0; grade < grades.size(); ++grade)
        inputs.looks[grade] = grades[grade].srv;
}

void VerifyNativeVanillaGradeRemoval(Device& device)
{
    NativeReferenceResources prepared;
    FO4HDR_WARP_CHECK(BuildNativeReferenceResources(
        device.device.Get(), prepared));
    if (!prepared.shader || !prepared.preblendShader) return;
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1),
        "Query native grade-removal context1");

    std::array<Texture3D, 4> grades;
    NativeReferenceInputs inputs;
    PrepareNativeGrades(device, grades, inputs);
    // D3D11.1 constant-buffer ranges use the production binding's
    // 16-constant granularity even though this shader reads two constants.
    inputs.constantCount = 16;

    const auto vertexShader = CreateVertexShader(
        *device.device.Get(), fo4hdr::generated::test::kFullscreenVs);
    const auto sampler = CreateClampPointSampler(*device.device.Get());
    auto output = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
    constexpr std::array<Float4, kWidth> corpus{{
        {0, 0, 0, 0.125F}, {0.00001F, 0.0000025F, 0, 0.25F},
        {0.18F, 0.04F, 0.01F, 0.375F}, {1, 0.25F, 0.0625F, 0.5F},
        {16, 4, 1, 0.625F}, {65504, 0, 0, 0.75F},
        {0, 65504, 0, 0.875F}, {0, 0, 65504, 1}}};
    D3D11_SUBRESOURCE_DATA data{corpus.data(), sizeof(corpus), 0};
    const auto scene = CreateTexture(*device.device.Get(),
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data);

    NativeGradeDraw rendering{device, prepared, vertexShader.Get(), sampler.Get(), output, scene};
    const auto render = [&](float strength, float luminanceRemoval, float colorRemoval) {
        return rendering.Run(strength, luminanceRemoval, colorRemoval);
    };
    constexpr std::array<std::array<float, 4>, 2> looks{{
        {{0, 1, 0, 0}}, {{0, 0, 0, 0}}}};
    constexpr std::array<std::array<float, 2>, 5> removals{{
        {{0, 0}}, {{0.35F, 0.65F}}, {{1, 0}}, {{0, 1}}, {{1, 1}}}};
    for (const auto& weights : looks)
    {
        std::array<Float4, 16> values{};
        values[1] = {weights[0], weights[1], weights[2], weights[3]};
        inputs.constants = CreateConstantBuffer(*device.device.Get(), values);
        DispatchNativeReferencePrepare(device.context.Get(), context1.Get(),
            inputs, prepared);
        for (const float strength : {0.0F, 0.5F, 1.0F})
        {
            const auto baseline = render(strength, 0, 0);
            for (const auto removal : removals)
            {
                const auto actual = render(
                    strength, removal[0], removal[1]);
                for (std::size_t index = 0; index < kWidth; ++index)
                {
                    CheckNativeRemovalPixel({corpus[index], baseline[index], actual[index], removal, index});
                }
            }
        }

        const auto baseline = render(1, 0, 0);
        const auto neutralized = render(1, 0, 1);
        const auto restored = render(1, 1, 0);
        CheckNativeGradeCorners({baseline, neutralized, restored}, corpus, weights[1] == 1);
    }
}

}  // namespace fo4hdr::tests::gpu
