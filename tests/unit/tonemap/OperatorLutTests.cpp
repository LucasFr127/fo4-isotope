// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/Aces2.hpp"
#include "Fallout4HDR/tonemap/Agx.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "reference/OperatorLutTestSupport.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

namespace fo4hdr::tests {
namespace {

constexpr double kReferencePeak{1000.0};
// Absolute per-channel interpolation budgets for this fixed binary32 corpus.
constexpr double kMaximumErrorNits{5.0};
constexpr double kMeanErrorNits{0.5};

struct AnalyticReference
{
    Aces2TransformHandle aces{CreateAces2Transform(kReferencePeak)};
    std::optional<AgxTransform> agx{CreateAgxTransform(kReferencePeak)};
    Gt7TransformHandle gt7{CreateGt7Transform(kReferencePeak)};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return aces && agx && gt7;
    }

    [[nodiscard]] LinearRgb Sample(ProductionToneMapperAlgorithm algorithm, LinearRgb scene) const noexcept
    {
        switch (algorithm)
        {
        case ProductionToneMapperAlgorithm::aces2Lmt: return MapAces2LmtToBt2020Nits(*aces, scene);
        case ProductionToneMapperAlgorithm::agX: return MapAgxToBt2020Nits(*agx, scene);
        case ProductionToneMapperAlgorithm::gt7: return MapGt7ToBt2020Nits(*gt7, scene);
        }
        return {};
    }
};

struct ErrorSummary
{
    double maximum{};
    double sum{};
    double darkPqCodes{};
    std::size_t channels{};

    void Add(LinearRgb actual, LinearRgb expected, bool nearBlack)
    {
        const std::array measured{actual.red, actual.green, actual.blue};
        const std::array reference{expected.red, expected.green, expected.blue};
        for (std::size_t channel = 0; channel < measured.size(); ++channel)
        {
            FO4HDR_CHECK(std::isfinite(measured[channel]));
            FO4HDR_CHECK(measured[channel] >= 0.0 && measured[channel] <= kReferencePeak);
            const auto error = std::abs(measured[channel] - reference[channel]);
            maximum = std::max(maximum, error);
            sum += error;
            ++channels;
            if (nearBlack)
                darkPqCodes = std::max(darkPqCodes, 1023.0 * std::abs(
                    EncodeSt2084Pq(measured[channel]) - EncodeSt2084Pq(reference[channel])));
        }
    }
};

void CheckSameRgb(LinearRgb actual, LinearRgb expected)
{
    FO4HDR_CHECK(actual.red == expected.red);
    FO4HDR_CHECK(actual.green == expected.green);
    FO4HDR_CHECK(actual.blue == expected.blue);
}

void CheckPacking(const OperatorLutData& lut)
{
    bool finite{true};
    bool alpha{true};
    std::size_t negativeCoefficients{};
    for (std::size_t index = 0; index < lut.rgba16.size(); ++index)
    {
        const auto bits = lut.rgba16[index];
        finite = finite && ((bits >> 10U) & 0x1FU) != 0x1FU;
        if (index % 4U == 3U)
            alpha = alpha && bits == 0x3C00U;
        else if (test::HalfToDouble(bits) < 0.0)
            ++negativeCoefficients;
    }
    FO4HDR_CHECK(finite);
    FO4HDR_CHECK(alpha);
    std::cout << "Operator LUT signed coefficients " << negativeCoefficients << '\n';
}

void CheckShaper(const OperatorLutData& lut)
{
    const auto parameters = OperatorLutShaperParameters(lut.algorithm, kReferencePeak);
    for (const auto value : parameters) FO4HDR_CHECK(std::isfinite(value));
    FO4HDR_CHECK(parameters[0] > 0.0F && parameters[3] > 0.0F);
    const auto width = lut.algorithm == ProductionToneMapperAlgorithm::gt7 ? 2.5 : 3.0;
    const auto coordinate = [&](double input) {
        const auto logarithm = std::log2(input + 1.0 / 65536.0);
        return std::atan((logarithm - parameters[2]) / width) * parameters[0] +
            logarithm * parameters[3] + parameters[1];
    };
    FO4HDR_CHECK(std::abs(coordinate(0.0)) <= 1.0e-5);
    FO4HDR_CHECK(std::abs(coordinate(65504.0) - 1.0) <= 1.0e-5);
    FO4HDR_CHECK(coordinate(0.0) < coordinate(0.18));
    FO4HDR_CHECK(coordinate(0.18) < coordinate(16.0));
    FO4HDR_CHECK(coordinate(16.0) < coordinate(65504.0));
}

void CheckInputPolicy(const OperatorLutData& lut)
{
    CheckSameRgb(test::SampleOperatorLut(lut, {}), {});
    CheckSameRgb(test::SampleOperatorLut(lut, {-1.0,
        std::numeric_limits<double>::quiet_NaN(),
        -std::numeric_limits<double>::infinity()}), {});
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto expected = lut.algorithm == ProductionToneMapperAlgorithm::aces2Lmt ? LinearRgb{} :
        test::SampleOperatorLut(lut, {65504.0, 65504.0, 65504.0});
    CheckSameRgb(test::SampleOperatorLut(lut, {infinity, infinity, infinity}), expected);
}

void CheckInterpolation(const OperatorLutData& lut, const AnalyticReference& analytic)
{
    ErrorSummary errors;
    const auto corpus = test::OperatorLutReferenceSamples();
    for (std::size_t index = 0; index < corpus.size(); ++index)
    {
        const auto scene = corpus[index];
        const auto raw = test::SampleOperatorLutRaw(lut, scene);
        FO4HDR_CHECK(std::isfinite(raw.red) && std::isfinite(raw.green) && std::isfinite(raw.blue));
        const auto contained = lut.algorithm == ProductionToneMapperAlgorithm::agX ?
            ApplyAgxDisplayGamut(raw, kReferencePeak) : ContainBt2020Nits(raw, kReferencePeak);
        const auto actual = test::SampleOperatorLut(lut, scene);
        if (index != 0U) CheckSameRgb(actual, contained);
        errors.Add(actual, analytic.Sample(lut.algorithm, scene), index < 4U);
    }
    const auto mean = errors.sum / static_cast<double>(errors.channels);
    FO4HDR_CHECK(errors.maximum <= kMaximumErrorNits);
    FO4HDR_CHECK(mean <= kMeanErrorNits);
    // PQ differences describe these four dark samples; they are not an acceptance gate.
    std::cout << "Operator LUT " << static_cast<unsigned>(lut.algorithm)
        << " max nits " << errors.maximum << " mean nits " << mean
        << " dark PQ codes " << errors.darkPqCodes << '\n';
}

void SetCornerCoefficients(OperatorLutData& lut, std::uint16_t value)
{
    const auto base = lut.edge - 2U;
    for (std::uint32_t tap = 0; tap < 64U; ++tap)
    {
        const auto x = base + tap % 4U;
        const auto y = base + (tap / 4U) % 4U;
        const auto z = base + tap / 16U;
        const auto texel = ((static_cast<std::size_t>(z) * lut.textureEdge + y) *
            lut.textureEdge + x) * 4U;
        for (std::size_t channel = 0; channel < 3U; ++channel)
            lut.rgba16[texel + channel] = value;
    }
}

void CheckSignedReconstruction(OperatorLutData& lut)
{
    // After the real-table comparisons, reuse its upper-corner support to
    // distinguish signed/above-peak reconstruction from premature clipping.
    const LinearRgb endpoint{65504.0, 65504.0, 65504.0};
    SetCornerCoefficients(lut, 0xBC00U);
    const auto raw = test::SampleOperatorLutRaw(lut, endpoint);
    FO4HDR_CHECK(std::abs(raw.red + 1.0) <= 1.0e-12);
    FO4HDR_CHECK(std::abs(raw.green + 1.0) <= 1.0e-12);
    FO4HDR_CHECK(std::abs(raw.blue + 1.0) <= 1.0e-12);
    CheckSameRgb(test::SampleOperatorLut(lut, endpoint), {});
    SetCornerCoefficients(lut, 0x67D0U);
    const auto abovePeak = test::SampleOperatorLutRaw(lut, endpoint);
    FO4HDR_CHECK(std::abs(abovePeak.red - 2000.0) <= 1.0e-9);
    FO4HDR_CHECK(std::abs(abovePeak.green - 2000.0) <= 1.0e-9);
    FO4HDR_CHECK(std::abs(abovePeak.blue - 2000.0) <= 1.0e-9);
    CheckSameRgb(test::SampleOperatorLut(lut, endpoint), {1000.0, 1000.0, 1000.0});
}

}  // namespace

void RunOperatorLutTests()
{
    FO4HDR_CHECK(test::HalfToDouble(0xBC00U) == -1.0);
    FO4HDR_CHECK(test::HalfToDouble(0x0001U) == std::ldexp(1.0, -24));
    const AnalyticReference analytic;
    FO4HDR_CHECK(analytic.IsValid());
    if (!analytic.IsValid()) return;
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2Lmt, ProductionToneMapperAlgorithm::agX,
             ProductionToneMapperAlgorithm::gt7})
    {
        auto lut = BuildOperatorLut(algorithm, kReferencePeak);
        FO4HDR_CHECK(lut.IsValid());
        if (!lut.IsValid()) continue;
        CheckPacking(lut);
        CheckShaper(lut);
        CheckInputPolicy(lut);
        CheckInterpolation(lut, analytic);
        if (algorithm == ProductionToneMapperAlgorithm::gt7) CheckSignedReconstruction(lut);
    }
}

}  // namespace fo4hdr::tests
