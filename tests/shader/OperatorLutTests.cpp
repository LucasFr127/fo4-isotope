// SPDX-License-Identifier: GPL-3.0-or-later

#include "d3d11/GpuHarness.hpp"
#include "ShaderTestSuite.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include "TestHarness.hpp"
#include "OperatorConstants.hpp"
#include "reference/OperatorLutTestSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace fo4hdr::tests::gpu {
namespace {

using Microsoft::WRL::ComPtr;
constexpr float peakNits = 1000;
constexpr float uiWhiteNits = 100;

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

D3D11_TEXTURE2D_DESC BatchDescription() noexcept
{
    D3D11_TEXTURE2D_DESC result{};
    result.Width = batchWidth;
    result.Height = batchHeight;
    result.MipLevels = result.ArraySize = result.SampleDesc.Count = 1;
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

    SamplingResources(ID3D11Device& device, const std::filesystem::path& shaderPath, const BatchPixels& pixels)
    {
        auto description = BatchDescription();
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
        const auto bytecode = ReadShader(shaderPath);
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
        const auto shaper = OperatorLutShaperParameters(table.algorithm, table.displayPeakNits);
        OperatorConstants values;
        values.mode = static_cast<std::uint32_t>(table.algorithm) + 1U;
        values.displayPeakNits = static_cast<float>(table.displayPeakNits);
        values.sceneReferenceWhiteNits = uiWhiteNits;
        values.operatorShaperAngleGain = shaper[0];
        values.operatorShaperBias = shaper[1];
        values.operatorShaperParameters = {shaper[2], shaper[3]};
        constants = ImmutableConstants(device, &values, sizeof(values));
    }
};

void BindSampling(ID3D11DeviceContext& context, const SamplingResources& resources, const SamplingTable& table)
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
}

void DispatchSampling(ID3D11DeviceContext& context, const SamplingResources& resources, const SamplingTable& table)
{
    BindSampling(context, resources, table);
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
    const SamplingResources resources(*device.device.Get(), directory / "OperatorLut.dxbc", inputs);
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2Lmt, ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7})
    {
        const auto generated = algorithm == ProductionToneMapperAlgorithm::gt7 ?
            OperatorLutData{} : BuildOperatorLut(algorithm, peakNits);
        const auto& table = algorithm == ProductionToneMapperAlgorithm::gt7 ? gt7 : generated;
        FO4HDR_CHECK(table.IsValid());
        if (!table.IsValid()) continue;
        const SamplingTable uploaded(*device.device.Get(), table);
        DispatchSampling(*device.context.Get(), resources, uploaded);
        CheckSampling(table, inputs, ReadSampling(*device.context.Get(), resources));
    }
}


using SamplingClock = std::chrono::steady_clock;
constexpr UINT samplingRepetitions = 128;

ComPtr<ID3D11Query> SamplingQuery(ID3D11Device& device, D3D11_QUERY kind)
{
    const D3D11_QUERY_DESC description{kind, 0};
    ComPtr<ID3D11Query> query;
    CheckHresult(device.CreateQuery(&description, &query), "Operator timing query");
    if (!query) throw std::runtime_error("Missing operator timing query");
    return query;
}

template <class T>
T ReadSamplingQuery(ID3D11DeviceContext& context, ID3D11Query& query, SamplingClock::time_point deadline)
{
    T value{};
    for (;;)
    {
        const auto result = context.GetData(&query, &value, static_cast<UINT>(sizeof(value)),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_OK) return value;
        if (result != S_FALSE) throw std::runtime_error("Operator timing query failed");
        if (SamplingClock::now() >= deadline)
            throw std::runtime_error("Operator timing query exceeded five seconds");
        std::this_thread::yield();
    }
}

double ReadSamplingMilliseconds(ID3D11DeviceContext& context, ID3D11Query& disjoint,
    ID3D11Query& start, ID3D11Query& end)
{
    const auto deadline = SamplingClock::now() + std::chrono::seconds(5);
    const auto frequency = ReadSamplingQuery<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(context, disjoint, deadline);
    if (frequency.Disjoint || frequency.Frequency == 0)
        throw std::runtime_error("Invalid operator timing frequency");
    const auto first = ReadSamplingQuery<UINT64>(context, start, deadline);
    const auto last = ReadSamplingQuery<UINT64>(context, end, deadline);
    if (last < first) throw std::runtime_error("Reversed operator timestamps");
    const double milliseconds = static_cast<double>(last - first) * 1000.0 /
        static_cast<double>(frequency.Frequency) / samplingRepetitions;
    if (!std::isfinite(milliseconds)) throw std::runtime_error("Nonfinite operator timing");
    return milliseconds;
}

double MeasureSamplingDispatches(ID3D11Device& device, ID3D11DeviceContext& context)
{
    const auto disjoint = SamplingQuery(device, D3D11_QUERY_TIMESTAMP_DISJOINT);
    const auto start = SamplingQuery(device, D3D11_QUERY_TIMESTAMP);
    const auto end = SamplingQuery(device, D3D11_QUERY_TIMESTAMP);
    for (UINT warmup = 0; warmup < 4; ++warmup) context.Dispatch(1, batchHeight, 1);
    context.Begin(disjoint.Get());
    context.End(start.Get());
    for (UINT iteration = 0; iteration < samplingRepetitions; ++iteration) context.Dispatch(1, batchHeight, 1);
    context.End(end.Get());
    context.End(disjoint.Get());
    context.Flush();
    return ReadSamplingMilliseconds(context, *disjoint.Get(), *start.Get(), *end.Get());
}

double SamplingMilliseconds(SamplingClock::time_point first, SamplingClock::time_point last)
{
    return std::chrono::duration<double, std::milli>(last - first).count();
}

void BenchmarkSamplingCase(const Device& device, const SamplingResources& resources,
    const BatchPixels& inputs, ProductionToneMapperAlgorithm algorithm)
{
    const auto bakeStart = SamplingClock::now();
    const auto table = BuildOperatorLut(algorithm, peakNits);
    const auto bakeEnd = SamplingClock::now();
    FO4HDR_CHECK(table.IsValid());
    if (!table.IsValid()) return;
    const auto uploadStart = SamplingClock::now();
    const SamplingTable uploaded(*device.device.Get(), table);
    const auto uploadEnd = SamplingClock::now();
    BindSampling(*device.context.Get(), resources, uploaded);
    const auto gpuMilliseconds = MeasureSamplingDispatches(*device.device.Get(), *device.context.Get());
    device.context->ClearState();
    CheckSampling(table, inputs, ReadSampling(*device.context.Get(), resources));
    // This measures the isolated wrapper and input/output traffic, not gameplay.
    std::cout << "Isolated WARP operator=" << ToString(algorithm) << " peak=" << peakNits
              << " colors=16 batch=4x4 warmup=4 repetitions=128 bake_cpu_ms="
              << SamplingMilliseconds(bakeStart, bakeEnd) << " upload_api_cpu_ms="
              << SamplingMilliseconds(uploadStart, uploadEnd) << " isolated_warp_batch_ms_per_dispatch="
              << gpuMilliseconds << '\n';
}

} // namespace

void BenchmarkOperatorLut(const std::filesystem::path& bytecodePath)
{
    const auto device = CreateWarpDevice();
    const auto inputs = SamplingInputs();
    const SamplingResources resources(*device.device.Get(), bytecodePath, inputs);
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2Lmt, ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7})
        BenchmarkSamplingCase(device, resources, inputs, algorithm);
}

void VerifyOperatorLut(const std::filesystem::path& fixtureDirectory)
{
    const auto table = BuildOperatorLut(ProductionToneMapperAlgorithm::gt7, peakNits);
    FO4HDR_CHECK(table.IsValid());
    if (table.IsValid()) VerifySamplingBatch(fixtureDirectory, table);
}

} // namespace fo4hdr::tests::gpu
