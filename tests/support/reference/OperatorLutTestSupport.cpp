// SPDX-License-Identifier: GPL-3.0-or-later

#include "OperatorLutTestSupport.hpp"

#include "Fallout4HDR/tonemap/Agx.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace fo4hdr::test {
namespace {

LinearRgb SanitizeInput(LinearRgb scene, ProductionToneMapperAlgorithm algorithm) noexcept
{
    const auto channel = [algorithm](double value) noexcept {
        if (std::isnan(value)) return 0.0;
        if (algorithm == ProductionToneMapperAlgorithm::aces2Lmt)
            if (!std::isfinite(value)) return 0.0;
        return std::clamp(value, 0.0, 65504.0);
    };
    return {channel(scene.red), channel(scene.green), channel(scene.blue)};
}

struct CubicAxis
{
    std::uint32_t base{};
    std::array<double, 4> weights{};
};

CubicAxis CoefficientAxis(double value, const OperatorLutData& lut) noexcept
{
    const auto shaper = OperatorLutShaperParameters(lut.algorithm, lut.displayPeakNits);
    const auto logarithm = std::log2(value + 1.0 / 65536.0);
    const auto width = lut.algorithm == ProductionToneMapperAlgorithm::gt7 ? 2.5 : 3.0;
    const auto coordinate = std::clamp(std::atan((logarithm - shaper[2]) / width) *
        shaper[0] + logarithm * shaper[3] + shaper[1], 0.0, 1.0);
    const auto position = coordinate * static_cast<double>(lut.edge - 1U);
    const auto base = std::min(static_cast<std::uint32_t>(std::floor(position)), lut.edge - 2U);
    const auto t = position - base;
    const auto t2 = t * t;
    const auto t3 = t2 * t;
    const auto oneMinusT = 1.0 - t;
    return {base, {oneMinusT * oneMinusT * oneMinusT / 6.0,
        (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0,
        (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0,
        t3 / 6.0}};
}

double Coefficient(const OperatorLutData& lut, const std::array<CubicAxis, 3>& axes,
    std::uint32_t tap, std::size_t channel) noexcept
{
    const auto x = tap % 4U;
    const auto y = (tap / 4U) % 4U;
    const auto z = tap / 16U;
    const auto texel = ((static_cast<std::size_t>(axes[2].base + z) * lut.textureEdge +
        axes[1].base + y) * lut.textureEdge + axes[0].base + x) * 4U;
    const auto weight = axes[0].weights[x] * axes[1].weights[y] * axes[2].weights[z];
    return weight * HalfToDouble(lut.rgba16[texel + channel]);
}

}  // namespace

double HalfToDouble(std::uint16_t bits) noexcept
{
    const auto negative = (bits & 0x8000U) != 0U;
    const auto exponent = (bits >> 10U) & 0x1FU;
    const auto mantissa = bits & 0x3FFU;
    double value{};
    if (exponent == 0U)
        value = std::ldexp(static_cast<double>(mantissa), -24);
    else if (exponent == 0x1FU)
        value = mantissa == 0U ? std::numeric_limits<double>::infinity() :
            std::numeric_limits<double>::quiet_NaN();
    else
        value = std::ldexp(1.0 + static_cast<double>(mantissa) / 1024.0,
            static_cast<int>(exponent) - 15);
    return negative ? -value : value;
}

LinearRgb SampleOperatorLutRaw(const OperatorLutData& lut, LinearRgb scene) noexcept
{
    if (!lut.IsValid()) return {};
    scene = SanitizeInput(scene, lut.algorithm);
    const std::array axes{CoefficientAxis(scene.red, lut),
        CoefficientAxis(scene.green, lut), CoefficientAxis(scene.blue, lut)};
    std::array<double, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel)
        for (std::uint32_t tap = 0; tap < 64U; ++tap)
            result[channel] += Coefficient(lut, axes, tap, channel);
    return {result[0], result[1], result[2]};
}

LinearRgb SampleOperatorLut(const OperatorLutData& lut, LinearRgb scene) noexcept
{
    if (!lut.IsValid()) return {};
    scene = SanitizeInput(scene, lut.algorithm);
    // Only full exact black bypasses quantized coefficient cancellation.
    if (std::max({scene.red, scene.green, scene.blue}) == 0.0) return {};
    const auto raw = SampleOperatorLutRaw(lut, scene);
    return lut.algorithm == ProductionToneMapperAlgorithm::agX ?
        ApplyAgxDisplayGamut(raw, lut.displayPeakNits) :
        ContainBt2020Nits(raw, lut.displayPeakNits);
}

std::array<LinearRgb, 16> OperatorLutReferenceSamples() noexcept
{
    return {{{0.0F, 0.0F, 0.0F}, {1.0e-6F, 1.0e-6F, 1.0e-6F},
        {1.0e-4F, 1.0e-4F, 1.0e-4F}, {1.0e-3F, 1.0e-3F, 1.0e-3F},
        {0.18F, 0.18F, 0.18F}, {1.0F, 1.0F, 1.0F}, {16.0F, 16.0F, 16.0F},
        {8.0F, 0.0F, 0.0F}, {0.0F, 8.0F, 0.0F}, {0.0F, 0.0F, 8.0F},
        {4.0F, 2.0F, 0.0F}, {2.03125F, 0.6875F, 0.53125F},
        {64.0F, 22.4F, 0.0F}, {65504.0F, 65504.0F, 65504.0F},
        {65504.0F, 0.0F, 0.0F}, {0.0F, 0.18F, 0.18F}}};
}

}  // namespace fo4hdr::test
