// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderReference.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/tonemap/Aces2.hpp"
#include "FO4Isotope/tonemap/Agx.hpp"
#include "FO4Isotope/tonemap/Gt7.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fo4hdr::tests::gpu {

namespace {

[[nodiscard]] std::uint32_t RoundMantissa(std::uint32_t truncated,
    std::uint32_t remainder, std::uint32_t halfway) noexcept
{
    return truncated + ((remainder > halfway ||
        (remainder == halfway && (truncated & 1U) != 0U)) ? 1U : 0U);
}

[[nodiscard]] std::uint16_t EncodeSubnormal(std::uint16_t sign,
    std::uint32_t mantissa, int exponent) noexcept
{
    if (exponent < -10) return sign;
    mantissa |= 0x800000U;
    const auto shift = static_cast<unsigned>(14 - exponent);
    const auto truncated = mantissa >> shift;
    const auto remainder = mantissa & ((1U << shift) - 1U);
    const auto halfway = 1U << (shift - 1U);
    return static_cast<std::uint16_t>(sign | RoundMantissa(truncated, remainder, halfway));
}

[[nodiscard]] std::uint16_t EncodeNormal(std::uint16_t sign,
    std::uint32_t mantissa, int exponent) noexcept
{
    auto rounded = RoundMantissa(mantissa >> 13U, mantissa & 0x1FFFU, 0x1000U);
    if (rounded == 0x400U) {
        rounded = 0U;
        ++exponent;
        if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    return static_cast<std::uint16_t>(sign |
        (static_cast<std::uint16_t>(exponent) << 10U) |
        static_cast<std::uint16_t>(rounded));
}

}  // namespace

[[nodiscard]] std::uint16_t FloatToBinary16(float value) noexcept
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto sourceExponent = (bits >> 23U) & 0xFFU;
    const auto mantissa = bits & 0x7FFFFFU;
    if (sourceExponent == 0xFFU) {
        return static_cast<std::uint16_t>(sign | 0x7C00U |
            (mantissa == 0U ? 0U : static_cast<std::uint16_t>((mantissa >> 13U) | 1U)));
    }
    const auto exponent = static_cast<int>(sourceExponent) - 127 + 15;
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
    if (exponent <= 0) return EncodeSubnormal(sign, mantissa, exponent);
    return EncodeNormal(sign, mantissa, exponent);
}
[[nodiscard]] float Binary16ToFloat(std::uint16_t bits) noexcept
{
    const auto negative = (bits & 0x8000U) != 0U;
    const auto exponent = (bits >> 10U) & 0x1FU;
    const auto mantissa = bits & 0x3FFU;
    float value{};
    if (exponent == 0U) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1FU) {
        value = mantissa == 0U ? INFINITY : NAN;
    } else {
        value = std::ldexp(
            1.0F + static_cast<float>(mantissa) / 1024.0F,
            static_cast<int>(exponent) - 15);
    }
    return negative ? -value : value;
}

[[nodiscard]] std::array<std::array<std::uint16_t, 4>, kWidth>
MakeFp16Pixels(const std::array<Float4, kWidth>& samples)
{
    std::array<std::array<std::uint16_t, 4>, kWidth> pixels{};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        pixels[index] = {
            FloatToBinary16(samples[index].red),
            FloatToBinary16(samples[index].green),
            FloatToBinary16(samples[index].blue),
            FloatToBinary16(samples[index].alpha),
        };
    }
    return pixels;
}

[[nodiscard]] std::array<std::array<std::uint8_t, 4>, kWidth>
MakeUnorm8Pixels(const std::array<Float4, kWidth>& samples)
{
    std::array<std::array<std::uint8_t, 4>, kWidth> pixels{};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto quantize = [](float value) {
            return static_cast<std::uint8_t>(std::lround(
                std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        pixels[index] = {
            quantize(samples[index].red),
            quantize(samples[index].green),
            quantize(samples[index].blue),
            quantize(samples[index].alpha),
        };
    }
    return pixels;
}

[[nodiscard]] unsigned CodeDistance(
    std::uint16_t left,
    std::uint16_t right) noexcept
{
    return left > right ? left - right : right - left;
}

[[nodiscard]] double Binary16LargestAdjacentStep(float value) noexcept
{
    const auto code = FloatToBinary16(value);
    const auto center = Binary16ToFloat(code);
    const auto above = Binary16ToFloat(
        static_cast<std::uint16_t>(code + 1U));
    const auto below = code == 0U
        ? center
        : Binary16ToFloat(static_cast<std::uint16_t>(code - 1U));
    return std::max(
        static_cast<double>(above - center),
        static_cast<double>(center - below));
}

[[nodiscard]] double Gt7Bt2020ErrorBound(
    fo4hdr::LinearRgb expectedNits,
    std::size_t channel) noexcept
{
    const std::array channels{
        expectedNits.red, expectedNits.green, expectedNits.blue};
    const auto largestNative = static_cast<float>(
        std::max({channels[0], channels[1], channels[2]}) / 100.0);
    const auto channelNative = static_cast<float>(channels[channel] / 100.0);
    // Allow two native FP16 steps plus independent half-bin rounding.
    // The largest channel bounds cancellation-sensitive cross-talk;
    // 100 converts native units to absolute nits.
    return 100.0 * 2.5 * std::max(
        Binary16LargestAdjacentStep(channelNative),
        Binary16LargestAdjacentStep(largestNative));
}

[[nodiscard]] fo4hdr::LinearRgb MapProductionOperator(
    TestedToneMapper toneMapper,
    fo4hdr::LinearRgb scene,
    float displayPeakNits) noexcept
{
    switch (toneMapper) {
    case TestedToneMapper::aces2: {
        const auto transform = fo4hdr::CreateAces2Transform(displayPeakNits);
        return transform ?
            fo4hdr::MapAdaptedAces2ToBt2020Nits(*transform, scene) :
            fo4hdr::LinearRgb{};
    }
    case TestedToneMapper::agX:
        return fo4hdr::MapAgxToBt2020Nits(scene, displayPeakNits);
    case TestedToneMapper::gt7:
        return fo4hdr::MapGt7ToBt2020Nits(scene, displayPeakNits);
    }
    return {};
}

[[nodiscard]] fo4hdr::ProductionToneMapperAlgorithm ProductionAlgorithm(
    TestedToneMapper toneMapper) noexcept
{
    switch (toneMapper) {
    case TestedToneMapper::aces2:
        return fo4hdr::ProductionToneMapperAlgorithm::aces2;
    case TestedToneMapper::agX:
        return fo4hdr::ProductionToneMapperAlgorithm::agX;
    case TestedToneMapper::gt7:
        return fo4hdr::ProductionToneMapperAlgorithm::gt7;
    }
    return static_cast<fo4hdr::ProductionToneMapperAlgorithm>(0xFF);
}

[[nodiscard]] std::uint32_t ProductionMode(
    TestedToneMapper toneMapper) noexcept
{
    switch (toneMapper) {
    case TestedToneMapper::aces2: return 1U;
    case TestedToneMapper::agX: return 2U;
    case TestedToneMapper::gt7: return 3U;
    }
    return 0U;
}

[[nodiscard]] double DecodeSrgb(double encoded) noexcept
{
    return encoded <= 0.04045
        ? encoded / 12.92
        : std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] unsigned TemporalHistoryCode(double encoded) noexcept
{
    return static_cast<unsigned>(std::lround(
        std::clamp(encoded, 0.0, 1.0) * 255.0));
}

[[nodiscard]] std::array<Float4, kWidth> UniformPixels(Float4 value) noexcept
{
    std::array<Float4, kWidth> result{};
    result.fill(value);
    return result;
}

}  // namespace fo4hdr::tests::gpu
