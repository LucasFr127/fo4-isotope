// SPDX-License-Identifier: MIT
// Copyright (c) 2023 Thatcher Freeman
// Test-only source oracle.
// Upstream: thatcherfreeman/utility-dctls@a495531de3e4704a0a6e0113daa69be303172ed0/Effects/Smooth Tetra Skin.dctl
// The upstream cylindrical conversion is attributed there to Juan Pablo
// Zambrano. This oracle intentionally retains separate interpolation passes
// and a per-evaluation plane fit so it remains independent of the optimized
// production preparation path.

// MIT License
//
// Copyright (c) 2023 Thatcher Freeman
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "SelectiveColorSourceOracle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace fo4hdr::tests::smooth_tetra_oracle {
namespace {

constexpr float kPi{std::numbers::pi_v<float>};
constexpr float kTwoPi{2.0F * kPi};
constexpr float kPolarSmoothness{5.0F};
constexpr float kNeutralSmoothness{0.1F};
constexpr std::array<float, kSelectiveColorRegionCount> kCenters{
    0.0F, kPi / 6.0F, kPi / 3.0F, 2.0F * kPi / 3.0F, kPi,
    4.0F * kPi / 3.0F, 5.0F * kPi / 3.0F};

struct Vec3
{
    float x{};
    float y{};
    float z{};
};

[[nodiscard]] float Wrap(float value) noexcept
{
    value = std::fmod(value, kTwoPi);
    return value < 0.0F ? value + kTwoPi : value;
}

[[nodiscard]] float Interpolate(
    float hue, const SelectiveColorPrepared& prepared,
    std::size_t lane) noexcept
{
    float numerator{};
    float denominator{};
    for (std::size_t index = 0; index < kCenters.size(); ++index)
    {
        const auto weight = std::exp(
            kPolarSmoothness * std::cos(hue - kCenters[index]));
        numerator += weight *
            static_cast<float>(prepared.regions[index][lane]);
        denominator += weight;
    }
    return numerator / denominator;
}

[[nodiscard]] Vec3 Unsmooth(
    float hue, float saturation,
    const SelectiveColorPrepared& prepared) noexcept
{
    const auto outputHue = hue + Interpolate(hue, prepared, 0);
    const auto outputSaturation =
        saturation * Interpolate(hue, prepared, 1);
    return {std::cos(outputHue) * outputSaturation,
        std::sin(outputHue) * outputSaturation,
        std::clamp(saturation, 0.0F, 2.0F) *
            Interpolate(hue, prepared, 2)};
}

[[nodiscard]] Vec3 MeanPlane(
    float hue, float saturation,
    const SelectiveColorPrepared& prepared) noexcept
{
    float sumXx{};
    float sumXy{};
    float sumYy{};
    Vec3 sumXz{};
    Vec3 sumYz{};
    for (const auto center : kCenters)
    {
        const auto x = std::cos(center);
        const auto y = std::sin(center);
        const auto output = Unsmooth(center, 1.0F, prepared);
        sumXx += x * x;
        sumXy += x * y;
        sumYy += y * y;
        sumXz.x += x * output.x;
        sumXz.y += x * output.y;
        sumXz.z += x * output.z;
        sumYz.x += y * output.x;
        sumYz.y += y * output.y;
        sumYz.z += y * output.z;
    }
    const auto denominator = sumXx * sumYy - sumXy * sumXy;
    const Vec3 a{
        (sumYy * sumXz.x - sumXy * sumYz.x) / denominator,
        (sumYy * sumXz.y - sumXy * sumYz.y) / denominator,
        (sumYy * sumXz.z - sumXy * sumYz.z) / denominator};
    const Vec3 b{
        (sumXx * sumYz.x - sumXy * sumXz.x) / denominator,
        (sumXx * sumYz.y - sumXy * sumXz.y) / denominator,
        (sumXx * sumYz.z - sumXy * sumXz.z) / denominator};
    const auto inputX = std::cos(hue) * saturation;
    const auto inputY = std::sin(hue) * saturation;
    return {a.x * inputX + b.x * inputY,
        a.y * inputX + b.y * inputY,
        a.z * inputX + b.z * inputY};
}

}  // namespace

[[nodiscard]] SelectiveColorRgb Apply(
    SelectiveColorRgb input,
    const SelectiveColorPrepared& prepared) noexcept
{
    const auto red = static_cast<float>(input.red);
    const auto green = static_cast<float>(input.green);
    const auto blue = static_cast<float>(input.blue);
    const auto mean = (red + green + blue) / 3.0F;
    if (mean == 0.0F) return input;
    const auto a = red - 0.5F * green - 0.5F * blue;
    const auto b = std::sqrt(3.0F) * 0.5F * (green - blue);
    const auto hue = Wrap(std::atan2(b / mean, a / mean));
    const auto saturation = std::hypot(a / mean, b / mean) / 3.0F;
    const auto unsmooth = Unsmooth(hue, saturation, prepared);
    const auto plane = MeanPlane(hue, saturation, prepared);
    const auto bump = std::exp2(-saturation * saturation /
        (kNeutralSmoothness * kNeutralSmoothness));
    const Vec3 outputAbz{
        std::lerp(unsmooth.x, plane.x, bump),
        std::lerp(unsmooth.y, plane.y, bump),
        std::lerp(unsmooth.z, plane.z, bump)};
    const auto outputHue = Wrap(std::atan2(outputAbz.y, outputAbz.x));
    const auto outputSaturation = std::hypot(outputAbz.x, outputAbz.y);
    const auto outputMean = mean * std::exp2(outputAbz.z);
    const auto radial = outputMean * outputSaturation;
    return {
        outputMean + 2.0F * radial * std::cos(outputHue),
        outputMean - radial * std::cos(outputHue) +
            std::sqrt(3.0F) * radial * std::sin(outputHue),
        outputMean - radial * std::cos(outputHue) -
            std::sqrt(3.0F) * radial * std::sin(outputHue)};
}

}  // namespace fo4hdr::tests::smooth_tetra_oracle
