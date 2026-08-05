// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "Fallout4HDR/config/HdrSettings.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;
using Rgb = std::array<double, 3>;
constexpr std::array<double, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};

Pixels ImageInputs()
{
    return {{{0, 0, 0, 0.75f}, {0.0078125f, 0.00390625f, 0.001953125f, 0.75f},
        {0.0625f, 0.03125f, 0.015625f, 0.75f}, {0.25f, 0.125f, 0.0625f, 0.75f},
        {8, 4, 2, 0.75f}, {1, 0.5f, 0.25f, 0.75f},
        {1, 1, 1, 0.75f}, {16, 8, 4, 0.75f}}};
}

double Luminance(const Rgb& color)
{
    return color[0] * 0.212500006 + color[1] * 0.715399981 + color[2] * 0.0720999986;
}

double SmoothMask(double value, double minimum, double maximum)
{
    const auto t = std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

void ScaleRgb(Rgb& color, double scale)
{
    for (auto& channel : color) channel = std::clamp(channel * scale, 0.0, 65504.0);
}

void ApplyBlacks(Rgb& color, double strength)
{
    const auto luminance = Luminance(color);
    if (luminance >= 0.02) return;
    const auto toe = 1 - std::max(luminance, 0.0) / 0.02;
    ScaleRgb(color, 1 + strength * toe * toe);
}

bool NeutralExceptBlacks(const HdrSettingsMath& controls)
{
    if (controls.shadowsEv != 0) return false;
    if (controls.highlightsEv != 0) return false;
    if (controls.contrastSlope != 1) return false;
    if (controls.saturationScale != 1) return false;
    return controls.colorBalanceBt709 == identity;
}

void ApplySaturation(Rgb& color, double requested)
{
    const auto luminance = Luminance(color);
    auto saturation = requested;
    if (requested > 1)
    {
        for (const auto channel : color)
        {
            const auto chroma = channel - luminance;
            if (chroma < 0)
                saturation = std::min(saturation, luminance / std::max(-chroma, 1.0e-6));
        }
    }
    for (auto& channel : color) channel = luminance + (channel - luminance) * saturation;
}

Rgb ApplyMatrix(const Rgb& color, const std::array<double, 9>& matrix)
{
    Rgb result{};
    for (std::size_t row = 0; row < result.size(); ++row)
        result[row] = std::clamp(matrix[row * 3] * color[0] + matrix[row * 3 + 1] * color[1] +
            matrix[row * 3 + 2] * color[2], 0.0, 65504.0);
    return result;
}

Rgb ReferenceImage(Float4 input, const HdrSettingsMath& controls)
{
    Rgb color{input.red, input.green, input.blue};
    if (NeutralExceptBlacks(controls))
    {
        ApplyBlacks(color, controls.blacksStrength);
        return color;
    }
    ScaleRgb(color, std::exp2(controls.shadowsEv * (1 - SmoothMask(Luminance(color), 0.02, 0.18))));
    ScaleRgb(color, std::exp2(controls.highlightsEv * SmoothMask(Luminance(color), 1, 4)));
    const auto luminance = Luminance(color);
    if (luminance > 0)
        ScaleRgb(color, 0.18 * std::pow(std::max(luminance / 0.18, 1.0e-6), controls.contrastSlope) / luminance);
    ApplyBlacks(color, controls.blacksStrength);
    ApplySaturation(color, controls.saturationScale);
    return ApplyMatrix(color, controls.colorBalanceBt709);
}

ComPtr<ID3D11Buffer> BindBorrowedConstants(PixelFixture& fixture)
{
    fixture.Bind(true);
    std::array<Float4, 32> values{};
    values[16] = {9, 8, 7, 6};
    auto buffer = ConstantBuffer(fixture.gpu.device.Get(), values);
    ComPtr<ID3D11DeviceContext1> context;
    CheckHresult(fixture.gpu.context.As(&context), "Image Context1");
    auto* bound = buffer.Get();
    const UINT first = 16;
    const UINT count = 16;
    context->PSSetConstantBuffers1(13, 1, &bound, &first, &count);
    return buffer;
}

void CheckBorrowedConstants(PixelFixture& fixture, ID3D11Buffer* expected)
{
    ComPtr<ID3D11DeviceContext1> context;
    CheckHresult(fixture.gpu.context.As(&context), "Image Context1");
    ComPtr<ID3D11Buffer> buffer;
    UINT first{};
    UINT count{};
    context->PSGetConstantBuffers(13, 1, &buffer);
    context->PSGetConstantBuffers1(13, 1, nullptr, &first, &count);
    FO4HDR_CHECK(buffer.Get() == expected);
    FO4HDR_CHECK(first == 16);
    FO4HDR_CHECK(count == 16);
}

void CheckImagePixel(Float4 actual, Float4 input, const HdrSettingsMath& controls)
{
    const auto expected = ReferenceImage(input, controls);
    const std::array channels{actual.red, actual.green, actual.blue};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        // One FP16 spacing plus the existing fixed scene arithmetic allowance.
        const auto allowance = std::abs(expected[channel]) / 1024 + 0.001;
        FO4HDR_CHECK(std::isfinite(channels[channel]));
        FO4HDR_CHECK(channels[channel] >= 0);
        FO4HDR_CHECK(channels[channel] <= 65504);
        FO4HDR_CHECK(std::abs(channels[channel] - expected[channel]) <= allowance);
    }
    FO4HDR_CHECK(actual.alpha == input.alpha);
}

void VerifyImageCase(const std::filesystem::path& directory, const HdrSettings& settings, bool neutral)
{
    const auto controls = DeriveHdrSettingsMath(settings);
    PixelFixture fixture(directory, width, height, {}, nullptr, {0, controls});
    const auto input = ImageInputs();
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, input);
    const auto native = BindPostProcess(fixture);
    const auto borrowed = BindBorrowedConstants(fixture);
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    CheckBorrowedConstants(fixture, borrowed.Get());
    CheckBoundInputs(fixture, 0, native.Get());
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    bool changed{};
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        CheckImagePixel(output[index], input[index], controls);
        const auto& actual = output[index];
        const auto& source = input[index];
        const auto same = actual.red == source.red && actual.green == source.green && actual.blue == source.blue;
        if (neutral) FO4HDR_CHECK(same);
        changed = changed || !same;
    }
    if (!neutral) FO4HDR_CHECK(changed);
}

void CheckInvalidColdTuple(const std::filesystem::path& directory)
{
    PixelFixture fixture(directory);
    ScenePassConfiguration configuration{width, height};
    configuration.sceneGrade.imageControls.colorBalanceBt709[0] = std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(!configuration.sceneGrade.Valid());
    ScenePassRenderer rejected;
    FO4HDR_CHECK(!rejected.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
}

}  // namespace

void VerifyImageControls(const std::filesystem::path& fixtureDirectory)
{
    VerifyImageCase(fixtureDirectory, {}, true);
    constexpr std::array members{&HdrSettings::contrast, &HdrSettings::blacks, &HdrSettings::highlights,
        &HdrSettings::shadows, &HdrSettings::saturationPercent, &HdrSettings::temperature, &HdrSettings::tint};
    for (const auto member : members)
    {
        HdrSettings settings;
        settings.*member = 50;
        VerifyImageCase(fixtureDirectory, settings, false);
    }
    CheckInvalidColdTuple(fixtureDirectory);
}

}  // namespace fo4hdr::tests::gpu
