// SPDX-License-Identifier: GPL-3.0-or-later

#include "d3d11/GpuHarness.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fo4hdr::tests::gpu {
namespace {

constexpr std::array<double, 3> weightedCodes{0.25, 0.5, 0.75};
constexpr double absoluteAllowance = 2.0e-5;
constexpr double relativeAllowance = 2.0e-5;

TransferPixels GradeTransferInputs()
{
    return {{{0, 0, 0, 1}, {0.18f, 0.18f, 0.18f, 1}, {1, 1, 1, 1},
        {2, 4, 1, 1}, {5.6f, 1, 0.25f, 1}, {16, 4, 2, 1},
        {65504, 32752, 1, 1}, {8, 0, 0, 1}, {0, 0.25f, 2, 1}}};
}

double ReferenceCurve(double value)
{
    return value * (0.14 * value + 1.0 / 60.0) /
        (value * (0.15 * value + 0.5) + 0.06);
}

double ReferenceScale(double peak)
{
    if (peak >= 5.6) return 1.0 / peak;
    const auto doubled = peak * 2;
    return 2 * (0.14 * doubled + 1.0 / 60.0) /
        ((doubled * (0.15 * doubled + 0.5) + 0.06) * ReferenceCurve(11.2));
}

std::array<double, 3> ReferenceTransfer(Float4 input, double strength)
{
    const std::array scene{static_cast<double>(input.red),
        static_cast<double>(input.green), static_cast<double>(input.blue)};
    const auto scale = ReferenceScale(std::max({scene[0], scene[1], scene[2]}));
    std::array<double, 3> endpoint;
    for (std::size_t channel = 0; channel < endpoint.size(); ++channel)
        endpoint[channel] = std::pow(weightedCodes[channel], 2.2) / scale;
    const auto compression = std::min(1.0, 65504.0 / std::max({endpoint[0], endpoint[1], endpoint[2], 65504.0}));
    for (std::size_t channel = 0; channel < endpoint.size(); ++channel)
        endpoint[channel] = std::lerp(scene[channel], std::min(endpoint[channel] * compression, 65504.0), strength);
    return endpoint;
}

void CheckTransferPixel(Float4 actual, Float4 input, double strength)
{
    const auto expected = ReferenceTransfer(input, strength);
    const std::array channels{actual.red, actual.green, actual.blue};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        FO4HDR_CHECK(std::isfinite(channels[channel]));
        FO4HDR_CHECK(channels[channel] >= 0);
        FO4HDR_CHECK(channels[channel] <= 65504);
        FO4HDR_CHECK(std::abs(static_cast<double>(channels[channel]) - expected[channel]) <=
            absoluteAllowance + relativeAllowance * std::abs(expected[channel]));
    }
    FO4HDR_CHECK(actual.alpha == 1);
    if (strength == 0)
    {
        FO4HDR_CHECK(actual.red == input.red);
        FO4HDR_CHECK(actual.green == input.green);
        FO4HDR_CHECK(actual.blue == input.blue);
    }
}

void VerifyStrengthPolicy(const std::filesystem::path& shaderPath, const TransferPixels& input)
{
    struct StrengthCase { float supplied; double normalized; };
    const std::array cases{StrengthCase{-1, 0}, StrengthCase{2, 1},
        StrengthCase{std::numeric_limits<float>::quiet_NaN(), 0}};
    for (const auto sample : cases)
    {
        const auto output = EvaluateTransfer(shaderPath, input, sample.supplied);
        for (std::size_t index = 0; index < output.size(); ++index)
            CheckTransferPixel(output[index], input[index], sample.normalized);
    }
}

}  // namespace

void VerifyHdrGradeTransfer(const std::filesystem::path& shaderPath)
{
    const auto input = GradeTransferInputs();
    for (const float strength : {0.0f, 0.5f, 1.0f})
    {
        const auto output = EvaluateTransfer(shaderPath, input, strength);
        for (std::size_t index = 0; index < output.size(); ++index)
            CheckTransferPixel(output[index], input[index], strength);
    }
    VerifyStrengthPolicy(shaderPath, input);
}

}  // namespace fo4hdr::tests::gpu
