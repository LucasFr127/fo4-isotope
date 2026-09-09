// SPDX-License-Identifier: GPL-3.0-or-later

#include "d3d11/GpuHarness.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using Microsoft::WRL::ComPtr;
using HalfPixel = std::array<std::uint16_t, 4>;
using CodePixel = std::array<std::uint8_t, 4>;

constexpr UINT calibrationWidth = 37;
constexpr UINT calibrationHeight = 3;
constexpr std::size_t calibrationPixels = calibrationWidth * calibrationHeight;
constexpr float calibrationPeak = 1000.0F;
constexpr float calibrationWhite = 150.0F;
constexpr CodePixel backdropCodes{128, 64, 32, 128};
constexpr CodePixel hudCodes{32, 64, 16, 128};

struct CalibrationConstants
{
    UINT width{calibrationWidth};
    UINT height{calibrationHeight};
    float peak{calibrationPeak};
    UINT domain{1};
    float white{calibrationWhite};
    UINT settingsPresent{};
    UINT mode{};
    UINT reserved{};
};
static_assert(sizeof(CalibrationConstants) == 32);
static_assert(offsetof(CalibrationConstants, mode) == 24);
static_assert(offsetof(CalibrationConstants, reserved) == 28);

D3D11_TEXTURE2D_DESC CalibrationDescription(DXGI_FORMAT format, UINT bindings)
{
    D3D11_TEXTURE2D_DESC result{};
    result.Width = calibrationWidth;
    result.Height = calibrationHeight;
    result.MipLevels = 1;
    result.ArraySize = 1;
    result.Format = format;
    result.SampleDesc.Count = 1;
    result.Usage = D3D11_USAGE_DEFAULT;
    result.BindFlags = bindings;
    return result;
}

ComPtr<ID3D11ShaderResourceView> CreateCalibrationInput(ID3D11Device& device,
    DXGI_FORMAT format, const D3D11_SUBRESOURCE_DATA& data)
{
    auto description = CalibrationDescription(format, D3D11_BIND_SHADER_RESOURCE);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    CheckHresult(device.CreateTexture2D(&description, &data, &texture), "Calibration input");
    CheckHresult(device.CreateShaderResourceView(texture.Get(), nullptr, &source), "Calibration SRV");
    return source;
}

std::vector<HalfPixel> CalibrationScene()
{
    // These binary16 encodings are exact: 4, 6.5 and 8 in the UI-relative domain.
    constexpr std::array<std::uint16_t, 3> levels{0x4400, 0x4680, 0x4800};
    std::vector<HalfPixel> result(calibrationPixels);
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const auto value = levels[(index % calibrationWidth) % levels.size()];
        result[index] = {value, value, value, 0x3C00};
    }
    return result;
}

std::vector<HalfPixel> CalibrationSettings()
{
    std::vector<HalfPixel> result(calibrationPixels);
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        // The first three columns retain zero coverage; the rest are half-white authored code.
        if (index % calibrationWidth >= 3) result[index] = {0x3800, 0x3800, 0x3800, 0x3800};
    }
    return result;
}

struct CalibrationResources
{
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> target;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> inputs;

    CalibrationResources(ID3D11Device& device, const std::filesystem::path& directory)
    {
        const auto bytecode = ReadShader(directory / "PresentPixels.dxbc");
        CheckHresult(device.CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader),
            "Calibration shipping shader");
        const auto description = CalibrationDescription(DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_UNORDERED_ACCESS);
        CheckHresult(device.CreateTexture2D(&description, nullptr, &output), "Calibration terminal");
        CheckHresult(device.CreateUnorderedAccessView(output.Get(), nullptr, &target), "Calibration UAV");
        PrepareInputs(device);
    }

    void PrepareInputs(ID3D11Device& device)
    {
        const auto scene = CalibrationScene();
        const auto settings = CalibrationSettings();
        const std::vector<CodePixel> backdrop(calibrationPixels, backdropCodes);
        const std::vector<CodePixel> hud(calibrationPixels, hudCodes);
        constexpr UINT halfPitch = calibrationWidth * sizeof(HalfPixel);
        constexpr UINT codePitch = calibrationWidth * sizeof(CodePixel);
        inputs[0] = CreateCalibrationInput(device, DXGI_FORMAT_R16G16B16A16_FLOAT,
            {scene.data(), halfPitch, 0});
        inputs[1] = CreateCalibrationInput(device, DXGI_FORMAT_R8G8B8A8_UNORM,
            {backdrop.data(), codePitch, 0});
        inputs[2] = CreateCalibrationInput(device, DXGI_FORMAT_R8G8B8A8_UNORM,
            {hud.data(), codePitch, 0});
        inputs[3] = CreateCalibrationInput(device, DXGI_FORMAT_R16G16B16A16_FLOAT,
            {settings.data(), halfPitch, 0});
    }
};

ComPtr<ID3D11Buffer> CalibrationBuffer(ID3D11Device& device, UINT mode)
{
    CalibrationConstants constants;
    constants.mode = mode;
    constants.settingsPresent = mode == 11 || mode == 14;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(constants);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{&constants, 0, 0};
    ComPtr<ID3D11Buffer> result;
    CheckHresult(device.CreateBuffer(&description, &data, &result), "Calibration constants");
    return result;
}

void DispatchCalibration(ID3D11DeviceContext& context, const CalibrationResources& resources,
    ID3D11Buffer* constants)
{
    std::array<ID3D11ShaderResourceView*, 4> inputs{};
    for (std::size_t index = 0; index < inputs.size(); ++index)
        inputs[index] = resources.inputs[index].Get();
    auto* target = resources.target.Get();
    context.CSSetShader(resources.shader.Get(), nullptr, 0);
    context.CSSetConstantBuffers(0, 1, &constants);
    context.CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
    context.CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context.Dispatch((calibrationWidth + 7) / 8, (calibrationHeight + 7) / 8, 1);
    target = nullptr;
    constants = nullptr;
    inputs.fill(nullptr);
    context.CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context.CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
    context.CSSetConstantBuffers(0, 1, &constants);
    context.CSSetShader(nullptr, nullptr, 0);
}

LinearRgb Neutral(double nits)
{
    return {nits, nits, nits};
}

LinearRgb WhitePatch(UINT x, UINT y)
{
    const double u = (x + 0.5) / calibrationWidth;
    const double v = (y + 0.5) / calibrationHeight;
    if (u < 0.2 || u > 0.8) return {};
    if (v < 0.2 || v > 0.8) return {};
    return Neutral(calibrationWhite);
}

LinearRgb GamutPatch(UINT x, UINT y)
{
    constexpr std::array<LinearRgb, 6> patches{{{150, 0, 0}, {0, 150, 0}, {0, 0, 150},
        {150, 150, 0}, {0, 150, 150}, {150, 0, 150}}};
    const auto patch = patches[x * patches.size() / calibrationWidth];
    if (y >= calibrationHeight / 2) return ConvertBt709ToBt2020(patch);
    return patch;
}

LinearRgb FlaggedScene(UINT x)
{
    constexpr std::array levels{4.0, 6.5, 8.0};
    const auto nits = levels[x % levels.size()] * calibrationWhite;
    if (nits >= calibrationPeak * (1.0 - 1e-5)) return {calibrationPeak, 0, 0};
    if (nits >= calibrationPeak * 0.9) return {calibrationPeak, calibrationPeak, 0};
    return Neutral(nits);
}

LinearRgb PatternLight(UINT mode, UINT x, UINT y)
{
    switch (mode)
    {
    case 10:
        return Neutral(std::floor(double(x) / (calibrationWidth - 1) * 31 + 1e-6) *
            (calibrationPeak * 1.125 / 31));
    case 11:
        return WhitePatch(x, y);
    case 12:
        {
            constexpr std::array stops{0.0, 0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0};
            return Neutral(stops[x * stops.size() / calibrationWidth]);
        }
    case 13:
        return GamutPatch(x, y);
    case 14:
        return FlaggedScene(x);
    default:
        throw std::runtime_error("Unexpected calibration case");
    }
}

LinearRgb BlendLight(LinearRgb behind, LinearRgb contribution, double coverage)
{
    return {contribution.red + behind.red * (1 - coverage),
        contribution.green + behind.green * (1 - coverage),
        contribution.blue + behind.blue * (1 - coverage)};
}

double DecodeCalibrationCode(double value)
{
    if (value <= 0.04045) return value / 12.92;
    return std::pow((value + 0.055) / 1.055, 2.4);
}

LinearRgb BlendNativeCodes(LinearRgb behind, CodePixel codes, bool backdrop)
{
    const auto coverage = codes[3] / 255.0;
    const double divisor = backdrop ? 1.0 : coverage;
    const double scale = backdrop ? calibrationWhite : calibrationWhite * coverage;
    const auto linear = ConvertBt709ToBt2020({
        DecodeCalibrationCode(codes[0] / 255.0 / divisor) * scale,
        DecodeCalibrationCode(codes[1] / 255.0 / divisor) * scale,
        DecodeCalibrationCode(codes[2] / 255.0 / divisor) * scale});
    return BlendLight(behind, linear, coverage);
}

LinearRgb ExpectedCalibration(UINT mode, UINT x, UINT y)
{
    auto light = PatternLight(mode, x, y);
    if (mode == 14)
    {
        light = BlendNativeCodes(light, backdropCodes, true);
        light = BlendNativeCodes(light, hudCodes, false);
    }
    if (mode != 11 && mode != 14) return light;
    if (x < 3) return light;
    // Exact half-white code unpremultiplies to white, then contributes half of active UI white.
    const auto settings = ConvertBt709ToBt2020(Neutral(calibrationWhite * 0.5));
    return BlendLight(light, settings, 0.5);
}

void CheckCalibrationPixel(std::uint32_t packed, LinearRgb light)
{
    const auto pq = EncodeSt2084Pq(ContainBt2020Nits(light, 10000.0));
    const std::array expected{QuantizeR10Unorm(pq.red), QuantizeR10Unorm(pq.green),
        QuantizeR10Unorm(pq.blue)};
    for (UINT channel = 0; channel < expected.size(); ++channel)
    {
        const auto actual = static_cast<int>((packed >> (10 * channel)) & 1023);
        // FP32 shipping arithmetic and the double reference may straddle one R10 rounding boundary.
        FO4HDR_CHECK(std::abs(actual - static_cast<int>(expected[channel])) <= 1);
    }
    FO4HDR_CHECK((packed >> 30) == 3);
}

void CheckCalibrationBatch(const Device& device, const CalibrationResources& resources, UINT mode)
{
    const auto constants = CalibrationBuffer(*device.device.Get(), mode);
    DispatchCalibration(*device.context.Get(), resources, constants.Get());
    const auto pixels = ReadPackedTexture(device, resources.output.Get());
    FO4HDR_CHECK(pixels.size() == calibrationPixels);
    if (pixels.size() != calibrationPixels) throw std::runtime_error("Calibration readback extent");
    for (UINT y = 0; y < calibrationHeight; ++y)
    {
        for (UINT x = 0; x < calibrationWidth; ++x)
            CheckCalibrationPixel(pixels[y * calibrationWidth + x], ExpectedCalibration(mode, x, y));
    }
}

}  // namespace

void VerifyDisplayCalibration(const std::filesystem::path& fixtureDirectory)
{
    const auto device = CreateWarpDevice();
    const CalibrationResources resources{*device.device.Get(), fixtureDirectory};
    for (UINT mode : {10U, 11U, 12U, 13U, 14U}) CheckCalibrationBatch(device, resources, mode);
}

}  // namespace fo4hdr::tests::gpu
