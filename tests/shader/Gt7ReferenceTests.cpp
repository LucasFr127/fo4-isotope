// SPDX-License-Identifier: GPL-3.0-or-later

#include "Gt7ReferenceTests.hpp"
#include "d3d11/GpuHarness.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

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

TransferPixels ReferenceInputs()
{
    return {{{0.5F, 1.23F, 0.75F, 1.0F},
        {12.3F, 34.3F, 56.9F, 1.0F},
        {1504.7F, 64.51F, 0.5F, 1.0F}, {},
        {8.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 8.0F, 16.0F, 1.0F},
        {0.0F, 0.60791015625F, 54.21875F, 1.0F},
        {0.0F, 2.6875F, 28.734375F, 1.0F},
        {65504.0F, 1.0F, 0.001F, 1.0F}}};
}

TransferPixels PolicyInputs()
{
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    return {{{nan, 1.0F, 4.0F, 0.0F}, {0.18F, nan, 4.0F, 0.0F},
        {0.18F, 1.0F, nan, 0.0F}, {-1.0F, -infinity, -1.0F, 0.0F},
        {infinity, 1.0F, 4.0F, 0.0F}, {0.18F, infinity, 4.0F, 0.0F},
        {0.18F, 1.0F, infinity, 0.0F}, {65505.0F, 1.0F, 4.0F, 0.0F},
        {65504.0F, 65504.0F, 65504.0F, 0.0F}}};
}

struct ReferenceCase
{
    float peak;
    bool crossTalk;
};

struct ReferenceChannel
{
    double expected;
    float actual;
    double vectorScaleBound;
    bool crossTalk;
};

bool AcceptChannel(const ReferenceChannel& sample)
{
    const auto& [expected, raw, vectorScaleBound, crossTalk] = sample;
    const auto actualHalf = FloatToBinary16(raw);
    const auto actual = Binary16ToFloat(actualHalf);
    const auto error = std::abs(static_cast<double>(actual) - expected);
    if (crossTalk) return actual > 0.0F && error <= vectorScaleBound;
    const auto distance = CodeDistance(actualHalf,
        FloatToBinary16(static_cast<float>(expected)));
    if (distance <= 2U) return true;
    if (expected > 1.0e-5) return false;
    return error <= 1.0e-4;
}

void ReportRejectedChannel(const ReferenceChannel& sample, float peak)
{
    const auto actualHalf = FloatToBinary16(sample.actual);
    const auto expectedHalf = FloatToBinary16(static_cast<float>(sample.expected));
    const auto decodedActual = Binary16ToFloat(actualHalf);
    const auto previousPrecision = std::cerr.precision(std::numeric_limits<double>::max_digits10);
    std::cerr << "GT7 reference rejection: peak=" << peak
              << " expected=" << sample.expected << " raw=" << sample.actual
              << " actual_half=" << actualHalf << " expected_half=" << expectedHalf
              << " decoded_half=" << decodedActual
              << " half_distance=" << CodeDistance(actualHalf, expectedHalf)
              << " cross_talk=" << sample.crossTalk
              << " vector_scale_bound=" << sample.vectorScaleBound
              << " raw_error=" << std::abs(static_cast<double>(sample.actual) - sample.expected)
              << " decoded_error=" << std::abs(static_cast<double>(decodedActual) - sample.expected)
              << '\n';
    std::cerr.precision(previousPrecision);
}

void CheckChannel(const ReferenceChannel& sample, float peak, double& maximumError)
{
    FO4HDR_CHECK(std::isfinite(sample.actual));
    FO4HDR_CHECK(sample.actual >= 0.0F);
    FO4HDR_CHECK(sample.actual <= peak / 100.0F);
    if (sample.crossTalk) FO4HDR_CHECK(sample.actual > 0.0F);
    if (!AcceptChannel(sample)) ReportRejectedChannel(sample, peak);
    FO4HDR_CHECK(AcceptChannel(sample));
    const auto error = std::abs(static_cast<double>(sample.actual) - sample.expected);
    maximumError = std::isfinite(error) ? std::max(maximumError, error) :
        std::numeric_limits<double>::infinity();
}

void CheckPixel(const Float4& input, const Float4& actual, ReferenceCase test,
    double& maximumError)
{
    const auto previousFailures = fo4hdr::test::failures;
    const auto expected = MapGt7ReferenceRec2020({input.red, input.green, input.blue}, test.peak);
    const auto largest = static_cast<float>(std::max({expected.red,
        expected.green, expected.blue}));
    const auto bound = 2.5 * Binary16LargestAdjacentStep(largest);
    const auto positiveCrossTalk = test.crossTalk && expected.red > 1.0e-5;
    CheckChannel({expected.red, actual.red, bound, positiveCrossTalk}, test.peak, maximumError);
    CheckChannel({expected.green, actual.green, bound, false}, test.peak, maximumError);
    CheckChannel({expected.blue, actual.blue, bound, false}, test.peak, maximumError);
    FO4HDR_CHECK(actual.alpha == 1.0F);
    if (fo4hdr::test::failures != previousFailures)
    {
        const auto previousPrecision = std::cerr.precision(std::numeric_limits<double>::max_digits10);
        std::cerr << "GT7 reference pixel: peak=" << test.peak
                  << " input=(" << input.red << ',' << input.green << ',' << input.blue << ")\n";
        std::cerr.precision(previousPrecision);
    }
}

void CheckCorpus(const TransferPixels& input, const TransferPixels& output,
    ReferenceCase test, double& maximumError)
{
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto positiveCrossTalk = test.crossTalk && index >= 6 && index <= 7;
        CheckPixel(input[index], output[index], {test.peak, positiveCrossTalk}, maximumError);
    }
}

TransferPixels CleanPolicyInputs()
{
    return {{{0.0F, 1.0F, 4.0F, 0.0F}, {0.18F, 0.0F, 4.0F, 0.0F},
        {0.18F, 1.0F, 0.0F, 0.0F}, {},
        {65504.0F, 1.0F, 4.0F, 0.0F}, {0.18F, 65504.0F, 4.0F, 0.0F},
        {0.18F, 1.0F, 65504.0F, 0.0F}, {65504.0F, 1.0F, 4.0F, 0.0F},
        {65504.0F, 65504.0F, 65504.0F, 0.0F}}};
}

void CheckPolicyPixel(const Float4& malformed, const Float4& control)
{
    for (const auto pixel : std::array{malformed, control})
    {
        for (float channel : {pixel.red, pixel.green, pixel.blue})
        {
            FO4HDR_CHECK(std::isfinite(channel));
            FO4HDR_CHECK(channel >= 0.0F && channel <= 10.0F);
        }
        FO4HDR_CHECK(pixel.alpha == 1.0F);
    }
    FO4HDR_CHECK(malformed.red == control.red);
    FO4HDR_CHECK(malformed.green == control.green);
    FO4HDR_CHECK(malformed.blue == control.blue);
}

void ReportPolicyWitness(const Float4& malformed, const Float4& control)
{
    const auto cpu = MapGt7ReferenceRec2020({0.0, 1.0, 4.0}, 1000.0);
    const auto previousPrecision = std::cout.precision(std::numeric_limits<double>::max_digits10);
    std::cout << "GT7 policy witness: peak=1000 index=0 malformed=("
              << malformed.red << ',' << malformed.green << ',' << malformed.blue
              << ") control=(" << control.red << ',' << control.green << ',' << control.blue
              << ") cpu=(" << cpu.red << ',' << cpu.green << ',' << cpu.blue
              << ") red_half=(" << FloatToBinary16(malformed.red) << ','
              << FloatToBinary16(control.red) << ',' << FloatToBinary16(static_cast<float>(cpu.red))
              << ") raw_red_minus_cpu=" << static_cast<double>(malformed.red) - cpu.red << '\n';
    std::cout.precision(previousPrecision);
}

void VerifyInputPolicy(const std::filesystem::path& shaderPath)
{
    const auto malformed = EvaluateTransfer(shaderPath, PolicyInputs(), 1000.0F);
    const auto control = EvaluateTransfer(shaderPath, CleanPolicyInputs(), 1000.0F);
    for (std::size_t index = 0; index < malformed.size(); ++index)
        CheckPolicyPixel(malformed[index], control[index]);
    ReportPolicyWitness(malformed[0], control[0]);
}

void CheckInvalidPeaks(const std::filesystem::path& shaderPath,
    const TransferPixels& input)
{
    for (float peak : {249.0F, std::numeric_limits<float>::infinity()})
    {
        const auto output = EvaluateTransfer(shaderPath, input, peak);
        for (const auto pixel : output)
        {
            FO4HDR_CHECK(pixel.red == 0.0F);
            FO4HDR_CHECK(pixel.green == 0.0F);
            FO4HDR_CHECK(pixel.blue == 0.0F);
            FO4HDR_CHECK(pixel.alpha == 1.0F);
        }
    }
}

}  // namespace

double VerifyGt7Reference(const std::filesystem::path& shaderPath)
{
    double maximumError{};
    const auto input = ReferenceInputs();
    for (float peak : {250.0F, 400.0F, 1000.0F, 10000.0F})
    {
        const auto output = EvaluateTransfer(shaderPath, input, peak);
        // Quantization retains a two-stored-FP16-code budget; it does not assert
        // two float ULPs for shader arithmetic or quantize the actual input.
        CheckCorpus(input, output, {peak, true}, maximumError);
    }
    VerifyInputPolicy(shaderPath);
    CheckInvalidPeaks(shaderPath, input);
    return maximumError;
}

}  // namespace fo4hdr::tests::gpu
