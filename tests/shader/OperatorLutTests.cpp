// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <span>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;
constexpr float peakNits = 1000;
constexpr float uiWhiteNits = 100;
constexpr Float4 coloredScene{2, 4, 1, 0.75f};

Pixels Uniform(Float4 color)
{
    Pixels result;
    result.fill(color);
    return result;
}

ComPtr<ID3D11ShaderResourceView> UniformTexture(PixelFixture& fixture, Float4 color)
{
    auto description = SceneDescription();
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const auto pixels = Uniform(color);
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * sizeof(Float4), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, &data, &texture), "Operator auxiliary");
    CheckHresult(fixture.gpu.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Operator auxiliary SRV");
    return view;
}

ComPtr<ID3D11ShaderResourceView> IdentityGrade(ID3D11Device* device)
{
    LutPixels pixels(lutSize * lutSize * lutSize);
    for (UINT z = 0; z < lutSize; ++z)
        for (UINT y = 0; y < lutSize; ++y)
            for (UINT x = 0; x < lutSize; ++x)
                pixels[LutIndex(x, y, z)] = {x / 15.0f, y / 15.0f, z / 15.0f, 1};
    return LutTexture(device, pixels);
}

struct BorrowedOperatorBindings
{
    PixelFixture& fixture;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> texture;

    explicit BorrowedOperatorBindings(PixelFixture& owner) : fixture(owner),
        texture(IdentityGrade(owner.gpu.device.Get()))
    {
        CheckHresult(owner.gpu.context.As(&context), "Operator context");
        std::array<Float4, 32> values{};
        values[16] = {9, 8, 7, 6};
        buffer = ConstantBuffer(owner.gpu.device.Get(), values);
        auto* constants = buffer.Get();
        const UINT first = 16;
        const UINT count = 16;
        context->PSSetConstantBuffers1(13, 1, &constants, &first, &count);
        auto* source = texture.Get();
        auto* sampler = owner.point.Get();
        context->PSSetShaderResources(7, 1, &source);
        context->PSSetSamplers(7, 1, &sampler);
        Check();
    }

    void Check()
    {
        ComPtr<ID3D11Buffer> actualBuffer;
        ComPtr<ID3D11ShaderResourceView> actualTexture;
        ComPtr<ID3D11SamplerState> actualSampler;
        UINT first{};
        UINT count{};
        context->PSGetConstantBuffers(13, 1, &actualBuffer);
        context->PSGetConstantBuffers1(13, 1, nullptr, &first, &count);
        context->PSGetShaderResources(7, 1, &actualTexture);
        context->PSGetSamplers(7, 1, &actualSampler);
        FO4HDR_CHECK(actualBuffer.Get() == buffer.Get());
        FO4HDR_CHECK(first == 16);
        FO4HDR_CHECK(count == 16);
        FO4HDR_CHECK(actualTexture.Get() == texture.Get());
        FO4HDR_CHECK(actualSampler.Get() == fixture.point.Get());
    }
};

void BindGrade(PixelFixture& fixture, ID3D11ShaderResourceView* identity)
{
    fixture.Bind(true);
    const std::array<ID3D11ShaderResourceView*, 7> views{
        fixture.renderer.Views(fixture.input).shaderResource, nullptr, nullptr,
        identity, identity, identity, identity};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    const std::array<ID3D11SamplerState*, 7> samplers{fixture.point.Get(), nullptr, nullptr,
        fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const std::array<Float4, 2> values{{{}, {1, 0, 0, 0}}};
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), values);
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
}

void CheckOperatorChannel(float actual, double expected)
{
    // Coarse integration allowance: two FP16 relative spacings and twice the
    // ordinary arithmetic floor. This is not a qualified LUT interpolation bound.
    const auto tolerance = std::abs(expected) * 2.0 / 1024.0 + 0.002;
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(actual >= 0);
    FO4HDR_CHECK(actual <= peakNits / uiWhiteNits);
    FO4HDR_CHECK(std::abs(static_cast<double>(actual) - expected) <= tolerance);
}

void CheckOperatorPixel(Float4 actual, Float4 source, const Gt7Transform& transform)
{
    const auto nits = MapGt7ToBt2020Nits(transform, {source.red, source.green, source.blue});
    CheckOperatorChannel(actual.red, nits.red / uiWhiteNits);
    CheckOperatorChannel(actual.green, nits.green / uiWhiteNits);
    CheckOperatorChannel(actual.blue, nits.blue / uiWhiteNits);
    FO4HDR_CHECK(actual.alpha == source.alpha);
    if (std::max({source.red, source.green, source.blue}) == 0)
    {
        FO4HDR_CHECK(actual.red == 0);
        FO4HDR_CHECK(actual.green == 0);
        FO4HDR_CHECK(actual.blue == 0);
    }
}

void VerifyDirectGrade(PixelFixture& fixture, BorrowedOperatorBindings& borrowed,
    const Gt7Transform& transform, Float4 color)
{
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, Uniform(color));
    BindGrade(fixture, borrowed.texture.Get());
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    borrowed.Check();
    const auto pixels = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : pixels) CheckOperatorPixel(pixel, color, transform);
}

void ProduceTone(PixelFixture& fixture, BorrowedOperatorBindings& borrowed)
{
    fixture.Bind(true);
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, Uniform(coloredScene));
    const auto zero = UniformTexture(fixture, {});
    const auto tag = UniformTexture(fixture, {0, 0, 0, 4.0f / 255.0f});
    const std::array<ID3D11ShaderResourceView*, 4> views{zero.Get(),
        fixture.renderer.Views(fixture.input).shaderResource, zero.Get(), tag.Get()};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    const std::array<ID3D11SamplerState*, 4> samplers{
        fixture.point.Get(), fixture.point.Get(), fixture.point.Get(), fixture.point.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const auto values = ToneConstants();
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), std::span{values}.first(5));
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
    FO4HDR_CHECK(DrawPass(fixture, 0, true) == SceneDrawOutcome::replaced);
    borrowed.Check();
    const auto pixels = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : pixels) CheckPixel(pixel, {2, 4, 1, 1});
}

struct TemporalInputs
{
    ComPtr<ID3D11Texture2D> metadata;
    ComPtr<ID3D11RenderTargetView> target;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> auxiliary;
};

float HistoryCode(float uiRelative)
{
    const auto fixedReference = uiRelative * uiWhiteNits / 203.0f;
    return std::pow(fixedReference / (1 + fixedReference), 1.0f / 2.2f);
}

float HistoryLuminance(Float4 color)
{
    return HistoryCode(color.green) * 0.5f + HistoryCode(color.blue) * 0.25f +
        HistoryCode(color.red) * 0.25f;
}

TemporalInputs BindTemporal(PixelFixture& fixture, float luminance)
{
    fixture.Bind(true);
    TemporalInputs result;
    auto description = SceneDescription();
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, nullptr, &result.metadata), "Operator metadata");
    CheckHresult(fixture.gpu.device->CreateRenderTargetView(result.metadata.Get(), nullptr, &result.target),
        "Operator metadata RTV");
    result.auxiliary[0] = UniformTexture(fixture, {luminance, 0.5f, 0, 0});
    result.auxiliary[1] = UniformTexture(fixture, {});
    result.auxiliary[2] = UniformTexture(fixture, {0.5f, 0, 0, 0});
    result.auxiliary[3] = UniformTexture(fixture, {});
    const std::array<ID3D11ShaderResourceView*, 5> views{fixture.renderer.Views(fixture.input).shaderResource,
        result.auxiliary[0].Get(), result.auxiliary[1].Get(), result.auxiliary[2].Get(), result.auxiliary[3].Get()};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    const std::array<ID3D11SamplerState*, 5> samplers{fixture.point.Get(), fixture.point.Get(),
        fixture.point.Get(), fixture.point.Get(), fixture.point.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const std::array<Float4, 6> values{{{1, 0, 1, 1}, {}, {1, 0, 0, 0},
        {0.25f, 0.5f, 0, 0}, {0.5f, 0.5f, 0, 1}, {0, 0, 1, 1}}};
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), values);
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
    const std::array<ID3D11RenderTargetView*, 2> targets{result.target.Get(),
        fixture.renderer.Views(fixture.destination).renderTarget};
    fixture.gpu.context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
    return result;
}

void VerifyTemporal(PixelFixture& fixture, BorrowedOperatorBindings& borrowed, std::size_t program)
{
    const auto before = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.input).texture);
    const auto luminance = HistoryLuminance(before[0]);
    const auto resources = BindTemporal(fixture, luminance);
    FO4HDR_CHECK(DrawPass(fixture, program, true) == SceneDrawOutcome::replaced);
    borrowed.Check();
    CheckSelection(fixture, fixture.destination);
    FO4HDR_CHECK(fixture.renderer.CurrentSceneDomain() == SceneColorDomain::bt2020UiRelative);
    const auto after = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : after) CheckPixel(pixel, {before[0].red, before[0].green, before[0].blue, 1});
    const auto metadata = ReadScene(fixture.gpu, resources.metadata.Get());
    const auto weight = program == 5 ? 0.0f : 0.475f;
    for (const auto pixel : metadata) CheckPixel(pixel, {luminance, weight, 0, 0});
}

void VerifyPublishedGrade(PixelFixture& fixture, BorrowedOperatorBindings& borrowed,
    const Gt7Transform& transform)
{
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    const auto raw = fixture.input;
    const auto tone = fixture.destination;
    ProduceTone(fixture, borrowed);
    fixture.input = tone;
    fixture.destination = AdditionalScene(fixture);
    BindGrade(fixture, borrowed.texture.Get());
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    borrowed.Check();
    CheckSelection(fixture, fixture.destination);
    FO4HDR_CHECK(fixture.renderer.CurrentSceneDomain() == SceneColorDomain::bt2020UiRelative);
    const auto grade = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : grade) CheckOperatorPixel(pixel, {2, 4, 1, 1}, transform);
    for (const auto program : {5u, 6u})
    {
        fixture.input = fixture.destination;
        fixture.destination = AdditionalScene(fixture);
        VerifyTemporal(fixture, borrowed, program);
    }
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    fixture.input = raw;
}

void VerifyInvalidTable(PixelFixture& fixture, const OperatorLutData& table)
{
    auto invalid = table;
    invalid.textureEdge = invalid.edge;
    FO4HDR_CHECK(!invalid.IsValid());
    ScenePassRenderer rejected;
    const ScenePassConfiguration configuration{width, height, fixture.whites, &invalid};
    FO4HDR_CHECK(!rejected.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    FO4HDR_CHECK(!rejected.Active());
}

}  // namespace

void VerifyOperatorLut(const std::filesystem::path& fixtureDirectory)
{
    const auto table = BuildOperatorLut(OperatorLutAlgorithm::gt7, peakNits);
    FO4HDR_CHECK(table.IsValid());
    if (!table.IsValid()) return;
    const auto transform = CreateGt7Transform(peakNits);
    FO4HDR_CHECK(transform != nullptr);
    if (!transform) return;
    PixelFixture fixture(fixtureDirectory, width, height, {uiWhiteNits, 203}, &table);
    BorrowedOperatorBindings borrowed(fixture);
    VerifyDirectGrade(fixture, borrowed, *transform, {0, 0, 0, 0.5f});
    VerifyDirectGrade(fixture, borrowed, *transform, coloredScene);
    VerifyPublishedGrade(fixture, borrowed, *transform);
    VerifyInvalidTable(fixture, table);
}

}  // namespace fo4hdr::tests::gpu
