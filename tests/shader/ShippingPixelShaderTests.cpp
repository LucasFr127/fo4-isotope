// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "d3d11/GpuHarness.hpp"
#include "HdrRenderer.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>

namespace fo4hdr::tests::gpu {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT width = 4;
constexpr UINT height = 2;
constexpr UINT lutSize = 16;
using Pixels = std::array<Float4, width * height>;
using HalfPixel = std::array<std::uint16_t, 4>;
using LutPixels = std::vector<Float4>;
constexpr Float4 sentinelColor{0.0625f, 0.125f, 0.25f, 0.5f};

D3D11_TEXTURE2D_DESC SceneDescription()
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return description;
}

bool ExactHalfRepresentation(std::uint32_t bits, int exponent)
{
    const auto positive = (bits >> 31) == 0;
    const auto normal = exponent > 0 && exponent < 31;
    const auto exact = (bits & 8191) == 0;
    return positive && normal && exact;
}
std::uint16_t ExactHalf(float value)
{
    if (value == 0.0f) return 0;
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = static_cast<int>((bits >> 23) & 255) - 127 + 15;
    if (!ExactHalfRepresentation(bits, exponent))
        throw std::runtime_error("Scene fixture requires exactly representable positive FP16");
    return static_cast<std::uint16_t>((exponent << 10) | ((bits >> 13) & 1023));
}

float DecodeHalf(std::uint16_t value)
{
    const auto exponent = (value >> 10) & 31;
    const auto fraction = value & 1023;
    const auto magnitude = exponent == 0 ? std::ldexp(static_cast<float>(fraction), -24) :
        std::ldexp(1.0f + static_cast<float>(fraction) / 1024.0f,
            static_cast<int>(exponent) - 15);
    if (exponent == 31) return std::numeric_limits<float>::quiet_NaN();
    return (value & 32768) != 0 ? -magnitude : magnitude;
}

void UploadScene(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const Pixels& pixels)
{
    std::array<HalfPixel, width * height> encoded{};
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        const auto& pixel = pixels[index];
        encoded[index] = {ExactHalf(pixel.red), ExactHalf(pixel.green),
            ExactHalf(pixel.blue), ExactHalf(pixel.alpha)};
    }
    context->UpdateSubresource(texture, 0, nullptr, encoded.data(), width * sizeof(HalfPixel), 0);
}

Pixels ReadScene(const Device& gpu, ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CheckHresult(gpu.device->CreateTexture2D(&description, nullptr, &staging), "Readback texture");
    gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    gpu.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Readback map");
    if (!mapped.pData || mapped.RowPitch < width * sizeof(HalfPixel))
    {
        gpu.context->Unmap(staging.Get(), 0);
        throw std::runtime_error("Invalid scene readback pitch");
    }
    Pixels output{};
    for (UINT row = 0; row < height; ++row)
    {
        std::array<HalfPixel, width> encoded{};
        std::memcpy(encoded.data(), static_cast<const char*>(mapped.pData) + row * mapped.RowPitch,
            sizeof(encoded));
        for (UINT column = 0; column < width; ++column)
        {
            const auto& pixel = encoded[column];
            output[row * width + column] = {DecodeHalf(pixel[0]), DecodeHalf(pixel[1]),
                DecodeHalf(pixel[2]), DecodeHalf(pixel[3])};
        }
    }
    gpu.context->Unmap(staging.Get(), 0);
    return output;
}

void CheckChannel(float actual, double expected)
{
    // One FP16 spacing plus a fixed arithmetic allowance; fixed before execution.
    const auto tolerance = std::abs(expected) / 1024.0 + 0.001;
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(std::abs(static_cast<double>(actual) - expected) <= tolerance);
}

void CheckPixel(const Float4& actual, const Float4& expected)
{
    CheckChannel(actual.red, expected.red);
    CheckChannel(actual.green, expected.green);
    CheckChannel(actual.blue, expected.blue);
    FO4HDR_CHECK(actual.alpha == expected.alpha);
}

ComPtr<ID3D11Buffer> ConstantBuffer(ID3D11Device* device, std::span<const Float4> values)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(values.size_bytes());
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    CheckHresult(device->CreateBuffer(&description, &data, &buffer), "Constant buffer");
    return buffer;
}

ComPtr<ID3D11ShaderResourceView> AuxiliaryTexture(ID3D11Device* device, const Pixels& pixels)
{
    auto description = SceneDescription();
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), width * sizeof(Float4), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    CheckHresult(device->CreateTexture2D(&description, &data, &texture), "Auxiliary texture");
    CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Auxiliary SRV");
    return view;
}

ComPtr<ID3D11SamplerState> Sampler(ID3D11Device* device, D3D11_FILTER filter)
{
    D3D11_SAMPLER_DESC description{};
    description.Filter = filter;
    description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.MaxLOD = D3D11_FLOAT32_MAX;
    description.MaxAnisotropy = 1;
    description.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> sampler;
    CheckHresult(device->CreateSamplerState(&description, &sampler), "Sampler");
    return sampler;
}

struct DrawCount
{
    unsigned count{};
    static void Draw(ID3D11DeviceContext* context, void* argument)
    {
        ++static_cast<DrawCount*>(argument)->count;
        context->Draw(3, 0);
    }
    static void CountOnly(ID3D11DeviceContext*, void* argument)
    {
        ++static_cast<DrawCount*>(argument)->count;
    }
};

struct PixelFixture
{
    Device gpu{CreateWarpDevice()};
    ScenePassRenderer renderer;
    SceneTextureHandle input;
    SceneTextureHandle destination;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> plain;
    ComPtr<ID3D11PixelShader> sentinel;
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> instance;
    ComPtr<ID3D11Buffer> sentinelConstants;
    ComPtr<ID3D11SamplerState> point{Sampler(gpu.device.Get(), D3D11_FILTER_MIN_MAG_MIP_POINT)};
    ComPtr<ID3D11SamplerState> linear{Sampler(gpu.device.Get(), D3D11_FILTER_MIN_MAG_MIP_LINEAR)};
    ComPtr<ID3D11RasterizerState> rasterizer;

    explicit PixelFixture(const std::filesystem::path& directory)
    {
        if (!renderer.Initialize(gpu.device.Get(), gpu.context.Get(), width, height))
            throw std::runtime_error("Scene renderer initialization failed");
        CheckHresult(renderer.CreateSceneTexture(SceneDescription(), nullptr, input), "Input scene");
        CheckHresult(renderer.CreateSceneTexture(SceneDescription(), nullptr, destination), "Output scene");
        const auto bytecode = ReadShader(directory / "Fullscreen.dxbc");
        CheckHresult(gpu.device->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, &vertex),
            "Fullscreen vertex shader");
        CheckHresult(gpu.device->CreateClassLinkage(&linkage), "Class linkage");
        plain = ObservedShader(directory / "ScenePassPlain.dxbc", nullptr);
        sentinel = ObservedShader(directory / "ScenePassSentinel.dxbc", linkage.Get());
        CheckHresult(linkage->CreateClassInstance("SentinelColor", 0, 0, 0, 0, &instance),
            "Sentinel class instance");
        const std::array color{sentinelColor};
        sentinelConstants = ConstantBuffer(gpu.device.Get(), color);
        D3D11_RASTERIZER_DESC description{};
        description.FillMode = D3D11_FILL_SOLID;
        description.CullMode = D3D11_CULL_NONE;
        description.DepthClipEnable = TRUE;
        CheckHresult(gpu.device->CreateRasterizerState(&description, &rasterizer), "Rasterizer");
    }

    ComPtr<ID3D11PixelShader> ObservedShader(const std::filesystem::path& path, ID3D11ClassLinkage* classes)
    {
        const auto bytes = ReadShader(path);
        const auto bytecode = std::as_bytes(std::span{bytes});
        if (FindRenoDxFallout4Shader(ComputeRenoDxShaderCrc32(bytecode)))
            throw std::runtime_error("Project fixture unexpectedly matches catalog");
        ComPtr<ID3D11PixelShader> shader;
        CheckHresult(renderer.CreateObservedPixelShader(bytecode, classes, &shader), "Observed fixture shader");
        return shader;
    }

    void Bind(bool dynamic = false)
    {
        const auto context = gpu.context.Get();
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->RSSetState(rasterizer.Get());
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        auto* classes = instance.Get();
        context->PSSetShader(dynamic ? sentinel.Get() : plain.Get(), dynamic ? &classes : nullptr, dynamic ? 1 : 0);
        auto* constants = sentinelConstants.Get();
        context->PSSetConstantBuffers(0, 1, &constants);
        auto* target = renderer.Views(destination).renderTarget;
        context->OMSetRenderTargets(1, &target, nullptr);
    }

    void CheckRestored(bool dynamic)
    {
        ComPtr<ID3D11PixelShader> shader;
        std::array<ID3D11ClassInstance*, 256> classes{};
        UINT count = static_cast<UINT>(classes.size());
        gpu.context->PSGetShader(&shader, classes.data(), &count);
        FO4HDR_CHECK(shader.Get() == (dynamic ? sentinel.Get() : plain.Get()));
        FO4HDR_CHECK(count == (dynamic ? 1u : 0u));
        if (dynamic && count == 1) FO4HDR_CHECK(classes[0] == instance.Get());
        FO4HDR_CHECK(count <= classes.size());
        for (UINT index = 0; index < std::min(count, static_cast<UINT>(classes.size())); ++index)
            if (classes[index]) classes[index]->Release();
    }
};

Pixels SceneInputs()
{
    return {{{2, 4, 1, 0.75f}, {4, 1, 0.5f, 0.75f}, {1, 3, 5, 0.75f}, {6, 2, 1, 0.75f},
        {3, 1, 2, 0.75f}, {1, 5, 3, 0.75f}, {4, 2, 6, 0.75f}, {2, 6, 0.5f, 0.75f}}};
}

Pixels BloomInputs()
{
    return {{{0.25f, 1.5f, 0.5f, 0}, {0.5f, 0.125f, 2, 0}, {1, 0.5f, 0.25f, 0}, {1.75f, 0.75f, 0.0625f, 0},
        {2, 0.25f, 1, 0}, {0.125f, 2, 0.75f, 0}, {0.75f, 1, 2, 0}, {1, 0.0625f, 0.5f, 0}}};
}

Pixels ExposureInputs()
{
    return {{{0.5f, 0, 0, 0}, {0.25f, 0, 0, 0}, {1.5f, 0, 0, 0}, {0.75f, 0, 0, 0},
        {1, 0, 0, 0}, {0.125f, 0, 0, 0}, {0.5f, 0, 0, 0}, {2, 0, 0, 0}}};
}

std::array<Float4, 6> ToneConstants()
{
    return {{{}, {4, 0.25f, 1.25f, 0}, {0.75f, 0, 0.5f, 1.5f},
        {0.5f, 0.75f, 1.25f, 0.25f}, {1, 0.5f, 0.5f, 1}, {0.125f, 0.25f, 0.5f, 0.25f}}};
}

Float4 ToneExpected(std::size_t variant, std::size_t index)
{
    const auto scene = SceneInputs()[index];
    if (index == 2 || index == 6) return {scene.red, scene.green, scene.blue, 1};
    const auto column = index % width;
    const auto row = index / width;
    const auto bloomColumn = variant < 2 ? column / 2 : column;
    const auto bloomRow = variant < 2 ? row : row / 2;
    const auto bloom = BloomInputs()[bloomRow * width + bloomColumn];
    const auto measured = ExposureInputs()[index].red;
    const auto exposure = std::clamp(1.25 / (0.00100000005 + measured), 0.25, 4.0);
    const std::array color{(scene.red + bloom.red) * exposure,
        (scene.green + bloom.green) * exposure, (scene.blue + bloom.blue) * exposure};
    const auto luminance = color[0] * 0.212500006 + color[1] * 0.715399981 + color[2] * 0.0720999986;
    const std::array shape{0.5, 0.75, 1.25};
    const std::array affine{0.125, 0.25, 0.5};
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel)
    {
        auto filtered = (color[channel] - luminance) * 0.75 + luminance;
        filtered += 0.25 * (luminance * shape[channel] - filtered);
        filtered = 0.5 * (1.5 * filtered - measured) + measured;
        if (variant == 1 || variant == 3) filtered += 0.25 * (affine[channel] - filtered);
        result[channel] = static_cast<float>(filtered);
    }
    return {result[0], result[1], result[2], 1};
}

void CheckBoundInputs(PixelFixture& fixture, UINT slot, ID3D11Buffer* constants)
{
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Buffer> buffer;
    fixture.gpu.context->PSGetShaderResources(slot, 1, &source);
    fixture.gpu.context->OMGetRenderTargets(1, &target, nullptr);
    fixture.gpu.context->PSGetConstantBuffers(2, 1, &buffer);
    FO4HDR_CHECK(source.Get() == fixture.renderer.Views(fixture.input).shaderResource);
    FO4HDR_CHECK(target.Get() == fixture.renderer.Views(fixture.destination).renderTarget);
    FO4HDR_CHECK(buffer.Get() == constants);
}

void VerifyTone(PixelFixture& fixture, std::size_t variant)
{
    const auto dynamic = variant == 0;
    fixture.Bind(dynamic);
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, SceneInputs());
    Pixels mask{};
    mask[2].alpha = mask[6].alpha = 4.0f / 255.0f;
    const auto bloom = AuxiliaryTexture(fixture.gpu.device.Get(), BloomInputs());
    const auto exposure = AuxiliaryTexture(fixture.gpu.device.Get(), ExposureInputs());
    const auto tags = AuxiliaryTexture(fixture.gpu.device.Get(), mask);
    const auto source = fixture.renderer.Views(fixture.input).shaderResource;
    std::array<ID3D11ShaderResourceView*, 4> resources{bloom.Get(), source, exposure.Get(), tags.Get()};
    if (variant >= 2) std::swap(resources[0], resources[1]);
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());
    const std::array<ID3D11SamplerState*, 4> samplers{fixture.point.Get(), fixture.point.Get(), fixture.point.Get(), fixture.point.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const auto values = ToneConstants();
    const auto count = variant == 1 || variant == 3 ? 6u : 5u;
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), std::span{values}.first(count));
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
    DrawCount draw;
    const auto& descriptor = kRenoDxFallout4Shaders[variant];
    const auto outcome = fixture.renderer.Execute(descriptor, fixture.input, fixture.destination, {DrawCount::Draw, &draw});
    FO4HDR_CHECK(outcome == SceneDrawOutcome::replaced);
    FO4HDR_CHECK(draw.count == 1);
    fixture.CheckRestored(dynamic);
    CheckBoundInputs(fixture, descriptor.sceneInputSlot, constants.Get());
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (std::size_t index = 0; index < output.size(); ++index) CheckPixel(output[index], ToneExpected(variant, index));
    FO4HDR_CHECK(output[0].red > 1.0f);
}

std::size_t LutIndex(UINT x, UINT y, UINT z)
{
    return (z * lutSize + y) * lutSize + x;
}

LutPixels LutInputs(unsigned variant)
{
    LutPixels pixels(lutSize * lutSize * lutSize);
    for (UINT z = 0; z < lutSize; ++z)
        for (UINT y = 0; y < lutSize; ++y)
            for (UINT x = 0; x < lutSize; ++x)
            {
                const auto u = x / 15.0f;
                const auto v = y / 15.0f;
                const auto w = z / 15.0f;
                pixels[LutIndex(x, y, z)] = {0.15f + 0.07f * variant + 0.3f * u * u + 0.11f * v + 0.08f * w * w,
                    0.2f + 0.04f * variant + 0.05f * u + 0.25f * v * v + 0.12f * w,
                    0.1f + 0.03f * variant + 0.09f * u * u + 0.08f * v + 0.3f * w * w, 1};
            }
    return pixels;
}

ComPtr<ID3D11ShaderResourceView> LutTexture(ID3D11Device* device, const LutPixels& pixels)
{
    D3D11_TEXTURE3D_DESC description{};
    description.Width = description.Height = description.Depth = lutSize;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), lutSize * sizeof(Float4), lutSize * lutSize * sizeof(Float4)};
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    CheckHresult(device->CreateTexture3D(&description, &data, &texture), "LUT texture");
    CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "LUT SRV");
    return view;
}

double Curve(double value)
{
    return value * (0.14 * value + 1.0 / 60.0) / (value * (0.15 * value + 0.5) + 0.06);
}

double GuideScale(double peak)
{
    if (peak >= 5.6) return 1.0 / peak;
    return 2.0 * (0.14 * (2.0 * peak) + 1.0 / 60.0) /
        (((2.0 * peak) * (0.15 * (2.0 * peak) + 0.5) + 0.06) * Curve(11.2));
}

std::array<double, 3> SampleLut(const LutPixels& pixels, const std::array<double, 3>& coordinate)
{
    std::array<UINT, 3> lower{};
    std::array<double, 3> fraction{};
    for (std::size_t axis = 0; axis < lower.size(); ++axis)
    {
        lower[axis] = static_cast<UINT>(std::floor(coordinate[axis]));
        fraction[axis] = coordinate[axis] - lower[axis];
    }
    std::array<double, 3> result{};
    for (UINT corner = 0; corner < 8; ++corner)
    {
        std::array<UINT, 3> index{};
        double weight = 1;
        for (UINT axis = 0; axis < 3; ++axis)
        {
            const auto upper = (corner >> axis) & 1;
            index[axis] = std::min(lower[axis] + upper, lutSize - 1);
            weight *= upper ? fraction[axis] : 1.0 - fraction[axis];
        }
        const auto& sample = pixels[LutIndex(index[0], index[1], index[2])];
        result[0] += weight * sample.red;
        result[1] += weight * sample.green;
        result[2] += weight * sample.blue;
    }
    return result;
}

Float4 LutExpected(const Float4& scene, const std::array<LutPixels, 4>& textures)
{
    const auto scale = GuideScale(std::max({scene.red, scene.green, scene.blue}));
    const std::array<double, 3> coordinates{15.0 * std::pow(std::clamp(scene.red * scale, 0.0, 1.0), 1.0 / 2.2),
        15.0 * std::pow(std::clamp(scene.green * scale, 0.0, 1.0), 1.0 / 2.2),
        15.0 * std::pow(std::clamp(scene.blue * scale, 0.0, 1.0), 1.0 / 2.2)};
    constexpr std::array weights{0.25, 0.5, 0.75, 0.125};
    std::array<double, 3> weighted{};
    for (std::size_t index = 0; index < textures.size(); ++index)
    {
        const auto sampled = SampleLut(textures[index], coordinates);
        for (std::size_t channel = 0; channel < weighted.size(); ++channel)
            weighted[channel] += weights[index] * sampled[channel];
    }
    return {static_cast<float>(std::pow(weighted[0], 2.2) / scale),
        static_cast<float>(std::pow(weighted[1], 2.2) / scale),
        static_cast<float>(std::pow(weighted[2], 2.2) / scale), scene.alpha};
}

void VerifyLut(PixelFixture& fixture)
{
    fixture.Bind();
    const Pixels scene{{{2, 0.75f, 0.125f, 0.375f}, {8, 2, 0.5f, 0.75f}, {0.5f, 2, 1, 0.5f}, {1, 4, 8, 0.25f},
        {1, 0.5f, 2, 0.375f}, {2, 8, 1, 0.75f}, {2, 1, 0.25f, 0.5f}, {0.5f, 1, 8, 0.25f}}};
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, scene);
    std::array<LutPixels, 4> pixels;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> luts;
    std::array<ID3D11ShaderResourceView*, 7> resources{};
    resources[0] = fixture.renderer.Views(fixture.input).shaderResource;
    for (unsigned index = 0; index < luts.size(); ++index)
    {
        pixels[index] = LutInputs(index);
        luts[index] = LutTexture(fixture.gpu.device.Get(), pixels[index]);
        resources[index + 3] = luts[index].Get();
    }
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());
    const std::array<ID3D11SamplerState*, 7> samplers{fixture.point.Get(), nullptr, nullptr,
        fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const std::array<Float4, 2> values{{{}, {0.25f, 0.5f, 0.75f, 0.125f}}};
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), values);
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
    DrawCount draw;
    FO4HDR_CHECK(fixture.renderer.Execute(kRenoDxFallout4Shaders[4], fixture.input, fixture.destination,
        {DrawCount::Draw, &draw}) == SceneDrawOutcome::replaced);
    FO4HDR_CHECK(draw.count == 1);
    fixture.CheckRestored(false);
    CheckBoundInputs(fixture, 0, constants.Get());
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (std::size_t index = 0; index < output.size(); ++index) CheckPixel(output[index], LutExpected(scene[index], pixels));
    FO4HDR_CHECK(output[1].red > 1.0f);
}

void VerifyFactory(PixelFixture& fixture)
{
    D3D11_TEXTURE2D_DESC actual{};
    fixture.renderer.Views(fixture.input).texture->GetDesc(&actual);
    FO4HDR_CHECK(actual.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    FO4HDR_CHECK(actual.Width == width && actual.Height == height);
    FO4HDR_CHECK(fixture.renderer.Views(fixture.input).shaderResource != nullptr);
    FO4HDR_CHECK(fixture.renderer.Views(fixture.input).renderTarget != nullptr);
    std::array<D3D11_TEXTURE2D_DESC, 4> invalid;
    invalid.fill(SceneDescription());
    invalid[0].Width += 1;
    invalid[1].MipLevels = 2;
    invalid[2].SampleDesc.Count = 2;
    invalid[3].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (const auto& description : invalid)
    {
        auto handle = fixture.input;
        FO4HDR_CHECK(fixture.renderer.CreateSceneTexture(description, nullptr, handle) == E_INVALIDARG);
        FO4HDR_CHECK(fixture.renderer.Views(handle).texture == nullptr);
    }
    SceneTextureHandle handle;
    const D3D11_SUBRESOURCE_DATA initial{};
    FO4HDR_CHECK(fixture.renderer.CreateSceneTexture(SceneDescription(), &initial, handle) == E_INVALIDARG);
    const std::array<std::byte, 4> invalidShader{};
    ComPtr<ID3D11PixelShader> shader;
    FO4HDR_CHECK(FAILED(fixture.renderer.CreateObservedPixelShader(invalidShader, nullptr, &shader)));
    FO4HDR_CHECK(!shader);
}

void CheckForwarded(PixelFixture& fixture, bool observed, bool dynamic = false)
{
    fixture.Bind(dynamic);
    DrawCount draw;
    const auto outcome = observed ? fixture.renderer.ExecuteObserved(fixture.input, fixture.destination,
        {DrawCount::Draw, &draw}) : fixture.renderer.Execute(kRenoDxFallout4Shaders[0], fixture.input,
        fixture.destination, {DrawCount::Draw, &draw});
    FO4HDR_CHECK(outcome == SceneDrawOutcome::forwarded);
    FO4HDR_CHECK(draw.count == 1);
    fixture.CheckRestored(dynamic);
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto& pixel : output) CheckPixel(pixel, sentinelColor);
}

void VerifyForwarding(PixelFixture& fixture)
{
    CheckForwarded(fixture, true);
    CheckForwarded(fixture, true, true);
    std::array<ID3D11ShaderResourceView*, 4> wrongSlots{};
    wrongSlots[0] = fixture.renderer.Views(fixture.input).shaderResource;
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(wrongSlots.size()), wrongSlots.data());
    CheckForwarded(fixture, false);
    fixture.Bind();
    auto description = SceneDescription();
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D11Texture2D> foreign;
    ComPtr<ID3D11RenderTargetView> target;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, nullptr, &foreign), "Foreign texture");
    CheckHresult(fixture.gpu.device->CreateRenderTargetView(foreign.Get(), nullptr, &target), "Foreign RTV");
    auto* source = fixture.renderer.Views(fixture.input).shaderResource;
    fixture.gpu.context->PSSetShaderResources(1, 1, &source);
    auto* foreignTarget = target.Get();
    fixture.gpu.context->OMSetRenderTargets(1, &foreignTarget, nullptr);
    DrawCount draw;
    FO4HDR_CHECK(fixture.renderer.Execute(kRenoDxFallout4Shaders[0], fixture.input, fixture.destination,
        {DrawCount::Draw, &draw}) == SceneDrawOutcome::forwarded);
    FO4HDR_CHECK(draw.count == 1);
    fixture.CheckRestored(false);
    const auto output = ReadScene(fixture.gpu, foreign.Get());
    for (const auto& pixel : output) CheckPixel(pixel, sentinelColor);
}

struct ReentrantDraw
{
    PixelFixture* fixture;
    unsigned count{};
    DrawCount nested;
    SceneDrawOutcome outcome{};
    static void Draw(ID3D11DeviceContext* context, void* argument)
    {
        auto& draw = *static_cast<ReentrantDraw*>(argument);
        ++draw.count;
        draw.outcome = draw.fixture->renderer.Execute(kRenoDxFallout4Shaders[0], draw.fixture->input,
            draw.fixture->destination, {DrawCount::CountOnly, &draw.nested});
        context->Draw(3, 0);
    }
};

void VerifyInvocationRejection(PixelFixture& fixture)
{
    DrawCount wrongThread;
    SceneDrawOutcome outcome{};
    std::thread worker([&] {
        outcome = fixture.renderer.Execute(kRenoDxFallout4Shaders[0], fixture.input, fixture.destination,
            {DrawCount::CountOnly, &wrongThread});
    });
    worker.join();
    FO4HDR_CHECK(outcome == SceneDrawOutcome::invalidInvocation);
    FO4HDR_CHECK(wrongThread.count == 0);
    fixture.Bind();
    auto* source = fixture.renderer.Views(fixture.input).shaderResource;
    fixture.gpu.context->PSSetShaderResources(1, 1, &source);
    ReentrantDraw draw{&fixture};
    FO4HDR_CHECK(fixture.renderer.ExecuteObserved(fixture.input, fixture.destination,
        {ReentrantDraw::Draw, &draw}) == SceneDrawOutcome::forwarded);
    FO4HDR_CHECK(draw.count == 1 && draw.nested.count == 0);
    FO4HDR_CHECK(draw.outcome == SceneDrawOutcome::invalidInvocation);
    fixture.CheckRestored(false);
    FO4HDR_CHECK(fixture.renderer.Retire());
    DrawCount stale;
    FO4HDR_CHECK(fixture.renderer.Execute(kRenoDxFallout4Shaders[0], fixture.input, fixture.destination,
        {DrawCount::CountOnly, &stale}) == SceneDrawOutcome::invalidInvocation);
    FO4HDR_CHECK(stale.count == 0);
    FO4HDR_CHECK(fixture.renderer.Views(fixture.input).texture == nullptr);
    FO4HDR_CHECK(fixture.renderer.Retire());
}

}  // namespace

void VerifyShippingPixelShaders(const std::filesystem::path& fixtureDirectory)
{
    PixelFixture fixture{fixtureDirectory};
    VerifyFactory(fixture);
    for (std::size_t variant = 0; variant < 4; ++variant) VerifyTone(fixture, variant);
    VerifyLut(fixture);
    VerifyForwarding(fixture);
    VerifyInvocationRejection(fixture);
    std::cout << "Checked scene factory, five shipping pixel programs and finite-draw state restoration\n";
}

}  // namespace fo4hdr::tests::gpu
