// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include "TestHarness.hpp"
#include "OperatorConstants.hpp"
#include "LiveSettings.hpp"
#include "reference/OperatorLutTestSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
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


constexpr UINT batchWidth = 4;
constexpr UINT batchHeight = 4;
using BatchPixels = std::array<Float4, batchWidth * batchHeight>;

struct BatchDimensions
{
    std::uint32_t width{batchWidth};
    std::uint32_t height{batchHeight};
    std::array<std::uint32_t, 2> reserved{};
};
static_assert(sizeof(BatchDimensions) == 16);

ComPtr<ID3D11Buffer> ImmutableConstants(ID3D11Device& device, const void* data, UINT size)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = size;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
    ComPtr<ID3D11Buffer> result;
    CheckHresult(device.CreateBuffer(&description, &initial, &result), "Operator batch constants");
    return result;
}

BatchPixels SamplingInputs()
{
    const auto samples = test::OperatorLutReferenceSamples();
    static_assert(std::tuple_size_v<decltype(samples)> == batchWidth * batchHeight);
    BatchPixels result{};
    for (std::size_t index = 0; index < samples.size(); ++index)
        result[index] = {static_cast<float>(samples[index].red), static_cast<float>(samples[index].green),
            static_cast<float>(samples[index].blue), 1};
    return result;
}

struct SamplingResources
{
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Buffer> dimensions;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;

    SamplingResources(ID3D11Device& device, const std::filesystem::path& directory, const BatchPixels& pixels)
    {
        auto description = SceneDescription(batchWidth, batchHeight);
        description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), batchWidth * sizeof(Float4), 0};
        CheckHresult(device.CreateTexture2D(&description, &data, &input), "Operator batch input");
        CheckHresult(device.CreateShaderResourceView(input.Get(), nullptr, &inputView), "Operator batch input SRV");
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        CheckHresult(device.CreateTexture2D(&description, nullptr, &output), "Operator batch output");
        CheckHresult(device.CreateUnorderedAccessView(output.Get(), nullptr, &outputView), "Operator batch output UAV");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHresult(device.CreateTexture2D(&description, nullptr, &staging), "Operator batch staging");
        const BatchDimensions size;
        dimensions = ImmutableConstants(device, &size, sizeof(size));
        const auto bytecode = ReadShader(directory / "OperatorLut.dxbc");
        CheckHresult(device.CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader),
            "Operator batch shader");
        D3D11_SAMPLER_DESC filter{};
        filter.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        filter.AddressU = filter.AddressV = filter.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        filter.MaxAnisotropy = 1;
        filter.ComparisonFunc = D3D11_COMPARISON_NEVER;
        CheckHresult(device.CreateSamplerState(&filter, &sampler), "Operator batch sampler");
    }
};

struct SamplingTable
{
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11Buffer> constants;

    SamplingTable(ID3D11Device& device, const OperatorLutData& table)
    {
        D3D11_TEXTURE3D_DESC description{};
        description.Width = description.Height = description.Depth = table.textureEdge;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const auto rowPitch = table.textureEdge * 8u;
        const D3D11_SUBRESOURCE_DATA data{table.rgba16.data(), rowPitch, rowPitch * table.textureEdge};
        CheckHresult(device.CreateTexture3D(&description, &data, &texture), "Operator coefficient texture");
        CheckHresult(device.CreateShaderResourceView(texture.Get(), nullptr, &view), "Operator coefficient SRV");
        const OperatorConstants values{static_cast<std::uint32_t>(table.algorithm) + 1u,
            static_cast<float>(table.displayPeakNits), uiWhiteNits, 0,
            OperatorLutShaperParameters(table.algorithm, table.displayPeakNits)};
        constants = ImmutableConstants(device, &values, sizeof(values));
    }
};

void DispatchSampling(ID3D11DeviceContext& context, const SamplingResources& resources, const SamplingTable& table)
{
    auto* dimensions = resources.dimensions.Get();
    auto* constants = table.constants.Get();
    auto* input = resources.inputView.Get();
    auto* coefficients = table.view.Get();
    auto* sampler = resources.sampler.Get();
    auto* output = resources.outputView.Get();
    context.CSSetShader(resources.shader.Get(), nullptr, 0);
    context.CSSetConstantBuffers(0, 1, &dimensions);
    context.CSSetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants);
    context.CSSetShaderResources(0, 1, &input);
    context.CSSetShaderResources(kProductionOperatorLutSlot, 1, &coefficients);
    context.CSSetSamplers(kProductionOperatorSamplerSlot, 1, &sampler);
    context.CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context.Dispatch(1, batchHeight, 1);
    // This context is owned by the batch; clear aliases before staging readback.
    context.ClearState();
}

BatchPixels ReadSampling(ID3D11DeviceContext& context, const SamplingResources& resources)
{
    context.CopyResource(resources.staging.Get(), resources.output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(resources.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Operator batch map");
    constexpr auto rowBytes = batchWidth * sizeof(Float4);
    if (!mapped.pData || mapped.RowPitch < rowBytes)
    {
        context.Unmap(resources.staging.Get(), 0);
        throw std::runtime_error("Invalid operator batch pitch");
    }
    BatchPixels result{};
    const auto* data = static_cast<const std::byte*>(mapped.pData);
    for (UINT row = 0; row < batchHeight; ++row)
        std::memcpy(result.data() + row * batchWidth, data + row * mapped.RowPitch, rowBytes);
    context.Unmap(resources.staging.Get(), 0);
    return result;
}

void CheckSamplingChannel(float actual, double expected)
{
    // CPU and GPU read the same packed coefficients. This provisional allowance
    // covers differing sampling arithmetic; it is not a proved hardware bound.
    const auto tolerance = 0.5 + std::abs(expected) / 1024.0;
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(actual >= 0);
    FO4HDR_CHECK(actual <= peakNits);
    FO4HDR_CHECK(std::abs(static_cast<double>(actual) - expected) <= tolerance);
}

void CheckSampling(const OperatorLutData& table, const BatchPixels& input, const BatchPixels& output)
{
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto& source = input[index];
        const auto expected = test::SampleOperatorLut(table, {source.red, source.green, source.blue});
        const auto actual = output[index];
        CheckSamplingChannel(actual.red, expected.red);
        CheckSamplingChannel(actual.green, expected.green);
        CheckSamplingChannel(actual.blue, expected.blue);
        FO4HDR_CHECK(actual.alpha == 1);
        if (std::max({source.red, source.green, source.blue}) == 0)
        {
            FO4HDR_CHECK(actual.red == 0);
            FO4HDR_CHECK(actual.green == 0);
            FO4HDR_CHECK(actual.blue == 0);
        }
    }
}

void VerifySamplingBatch(const std::filesystem::path& directory, const OperatorLutData& gt7)
{
    const auto device = CreateWarpDevice();
    const auto inputs = SamplingInputs();
    const SamplingResources resources(*device.device.Get(), directory, inputs);
    for (const auto algorithm : {OperatorLutAlgorithm::aces2, OperatorLutAlgorithm::agX, OperatorLutAlgorithm::gt7})
    {
        const auto generated = algorithm == OperatorLutAlgorithm::gt7 ?
            OperatorLutData{} : BuildOperatorLut(algorithm, peakNits);
        const auto& table = algorithm == OperatorLutAlgorithm::gt7 ? gt7 : generated;
        FO4HDR_CHECK(table.IsValid());
        if (!table.IsValid()) continue;
        const SamplingTable uploaded(*device.device.Get(), table);
        DispatchSampling(*device.context.Get(), resources, uploaded);
        CheckSampling(table, inputs, ReadSampling(*device.context.Get(), resources));
    }
}


class LiveConfigurationFile
{
public:
    LiveConfigurationFile()
    {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
            ("Fallout4HDR-live-settings-" + std::to_string(token));
        if (!std::filesystem::create_directory(directory_))
            throw std::runtime_error("Cannot create live settings directory");
    }

    ~LiveConfigurationFile()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    PluginConfigurationResult Load() const
    {
        const auto path = directory_ / "settings.ini";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "[Output]\nDisplayPeakNits=1000\nUiPaperWhiteNits=100\nVideoWhiteNits=203\n"
                  "[ToneMapper]\nAlgorithm=GT7\n";
        output.close();
        if (!output) throw std::runtime_error("Cannot write live settings input");
        return LoadPluginConfiguration(path);
    }

private:
    std::filesystem::path directory_;
};

void CheckLiveTuple(const LiveSettingsSnapshot& snapshot, const PluginConfiguration& active,
    const PluginConfiguration& requested, bool pending)
{
    FO4HDR_CHECK(snapshot.available);
    FO4HDR_CHECK(snapshot.active == active);
    FO4HDR_CHECK(snapshot.requested == requested);
    FO4HDR_CHECK(snapshot.pending == pending);
    FO4HDR_CHECK(snapshot.activeGeneration == (pending ? 1u : 2u));
    FO4HDR_CHECK(snapshot.requestedGeneration == 2);
}

void VerifyLiveChange(LiveSettings& live, PixelFixture& fixture, const PluginConfiguration& initial)
{
    auto desired = initial;
    desired.display.uiPaperWhiteNits = 150;
    desired.display.videoWhiteNits = 250;
    desired.display.vanillaColorGradingStrengthPercent = 50;
    FO4HDR_CHECK(live.Request(desired) == LiveSettingsRequestStatus::accepted);
    auto invalid = desired;
    invalid.display.displayPeakNits = 299;
    FO4HDR_CHECK(live.Request(invalid) == LiveSettingsRequestStatus::invalid);
    CheckLiveTuple(live.Snapshot(), initial, desired, true);
    FO4HDR_CHECK(!live.CommitAtClosedFrame(PresentationScope::unavailable));
    CheckLiveTuple(live.Snapshot(), initial, desired, true);
    FO4HDR_CHECK(fixture.renderer.MatchesLayerWhites({100, 203}));
    FO4HDR_CHECK(live.Snapshot().error == LiveSettingsError::applyRejected);
    FO4HDR_CHECK(!live.RequestSave());
    FO4HDR_CHECK(live.Snapshot().persistence.state == SettingsPersistenceState::unavailable);
    FO4HDR_CHECK(live.Snapshot().saveRejected);
    FO4HDR_CHECK(live.CommitAtClosedFrame(PresentationScope::qualifiedExclusive));
    CheckLiveTuple(live.Snapshot(), desired, desired, false);
    FO4HDR_CHECK(live.Snapshot().error == LiveSettingsError::none);
    FO4HDR_CHECK(live.Snapshot().saveRejected);
    FO4HDR_CHECK(fixture.renderer.MatchesLayerWhites({150, 250}));
    FO4HDR_CHECK(!fixture.renderer.MatchesLayerWhites({100, 203}));
}

void ProduceLiveGrade(PixelFixture& fixture, BorrowedOperatorBindings& borrowed,
    const LineageScenes& scenes)
{
    fixture.input = scenes.raw;
    fixture.destination = scenes.tone;
    ProduceTone(fixture, borrowed);
    fixture.input = scenes.tone;
    fixture.destination = scenes.grade;
    BindGrade(fixture, borrowed.texture.Get());
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    borrowed.Check();
    CheckSelection(fixture, scenes.grade);
    FO4HDR_CHECK(fixture.renderer.CurrentSceneDomain() == SceneColorDomain::bt2020UiRelative);
}

void CheckUnchangedPixels(const std::vector<Float4>& actual, const std::vector<Float4>& expected)
{
    FO4HDR_CHECK(actual.size() == expected.size());
    if (actual.size() != expected.size()) return;
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        FO4HDR_CHECK(actual[index].red == expected[index].red);
        FO4HDR_CHECK(actual[index].green == expected[index].green);
        FO4HDR_CHECK(actual[index].blue == expected[index].blue);
        FO4HDR_CHECK(actual[index].alpha == expected[index].alpha);
    }
}

void VerifyLiveRefresh(PixelFixture& fixture, BorrowedOperatorBindings& borrowed,
    const LineageScenes& scenes, const std::vector<Float4>& before)
{
    const auto retained = ReadScene(fixture.gpu, fixture.renderer.Views(scenes.grade).texture);
    CheckUnchangedPixels(retained, before);
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    fixture.input = scenes.grade;
    fixture.destination = AdditionalScene(fixture);
    const auto staleInputs = BindTemporal(fixture, HistoryLuminance(before[0]));
    FO4HDR_CHECK(DrawPass(fixture, 5, true) == SceneDrawOutcome::forwarded);
    borrowed.Check();
    CheckSelection(fixture, {});
    ProduceLiveGrade(fixture, borrowed, scenes);
    const auto refreshed = ReadScene(fixture.gpu, fixture.renderer.Views(scenes.grade).texture);
    // Reuse the actual previous FP16 result: only UI-relative storage changes.
    for (const auto pixel : refreshed)
        CheckPixel(pixel, {before[0].red * 100 / 150, before[0].green * 100 / 150,
            before[0].blue * 100 / 150, 1});
    FO4HDR_CHECK(fixture.renderer.EndFrame());
}

void VerifyLiveSettings(const std::filesystem::path& directory, const OperatorLutData& table)
{
    const LiveConfigurationFile input;
    const auto loaded = input.Load();
    FO4HDR_CHECK(loaded.status == PluginConfigurationStatus::loaded);
    if (loaded.status != PluginConfigurationStatus::loaded) return;
    auto prepared = std::make_unique<OperatorLutBuildResult>();
    prepared->request = {loaded.value.algorithm, loaded.value.display.displayPeakNits, 1};
    prepared->data = table;
    const LayerWhiteConfiguration whites{static_cast<float>(loaded.value.display.uiPaperWhiteNits),
        static_cast<float>(loaded.value.display.videoWhiteNits)};
    PixelFixture fixture(directory, width, height, whites, &prepared->data);
    PresentationPass presentation;
    const PresentationConfiguration configuration{width, height,
        static_cast<float>(loaded.value.display.displayPeakNits), &fixture.renderer, whites};
    FO4HDR_CHECK(presentation.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    OperatorLutWorker worker;
    SettingsPersistence persistence;
    LiveSettings live;
    FO4HDR_CHECK(live.Initialize(loaded.value, prepared, {worker, presentation, persistence},
        PresentationScope::qualifiedExclusive));
    FO4HDR_CHECK(!prepared);
    FO4HDR_CHECK(live.Snapshot().available);
    FO4HDR_CHECK(live.Snapshot().active == loaded.value);
    FO4HDR_CHECK(live.Snapshot().activeGeneration == 1);
    FO4HDR_CHECK(!live.Snapshot().pending);
    if (!live.Snapshot().available) return;
    BorrowedOperatorBindings borrowed(fixture);
    const LineageScenes scenes{fixture.input, fixture.destination, AdditionalScene(fixture), {}};
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    ProduceLiveGrade(fixture, borrowed, scenes);
    const auto before = ReadScene(fixture.gpu, fixture.renderer.Views(scenes.grade).texture);
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    VerifyLiveChange(live, fixture, loaded.value);
    borrowed.Check();
    VerifyLiveRefresh(fixture, borrowed, scenes, before);
    FO4HDR_CHECK(live.Retire());
    FO4HDR_CHECK(!live.Snapshot().available);
    FO4HDR_CHECK(presentation.Retire());
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
    VerifySamplingBatch(fixtureDirectory, table);
    VerifyLiveSettings(fixtureDirectory, table);
}

}  // namespace fo4hdr::tests::gpu
