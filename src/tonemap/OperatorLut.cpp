// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/tonemap/OperatorLut.hpp"

#include "Fallout4HDR/tonemap/Aces2.hpp"
#include "Fallout4HDR/tonemap/Agx.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

namespace fo4hdr {
namespace {

constexpr std::uint32_t kAces2OperatorLutEdge{128};
constexpr std::uint32_t kAgxOperatorLutEdge{92};
constexpr std::uint32_t kGt7OperatorLutEdge{64};
constexpr std::uint32_t kOperatorLutCoefficientPadding{2};
constexpr double kOperatorLutSceneMaximum{65504.0};

double OperatorLutInputOffset(OperatorLutAlgorithm algorithm) noexcept;
std::uint32_t OperatorLutEdge(OperatorLutAlgorithm algorithm) noexcept;
double OperatorLutInputShaperMinimum(OperatorLutAlgorithm algorithm, double peak) noexcept;
double OperatorLutInputShaperMaximum(OperatorLutAlgorithm algorithm, double peak) noexcept;
double DecodeOperatorLutCoordinate(
    double coordinate, OperatorLutAlgorithm algorithm, double peak) noexcept;

[[nodiscard]] bool RoundMantissaUp(
    std::uint32_t remainder, std::uint32_t halfway,
    std::uint32_t mantissa) noexcept
{
    return remainder > halfway ||
        (remainder == halfway && (mantissa & 1U) != 0U);
}

[[nodiscard]] std::uint16_t SubnormalHalfMantissa(
    int exponent, std::uint32_t mantissa) noexcept
{
    mantissa |= 0x00800000U;
    const auto shift = static_cast<unsigned>(-exponent - 14);
    const auto roundShift = shift + 13U;
    auto halfMantissa = mantissa >> roundShift;
    const auto remainderMask = (std::uint32_t{1} << roundShift) - 1U;
    const auto remainder = mantissa & remainderMask;
    const auto halfway = std::uint32_t{1} << (roundShift - 1U);
    if (RoundMantissaUp(remainder, halfway, halfMantissa)) ++halfMantissa;
    return static_cast<std::uint16_t>(halfMantissa);
}

[[nodiscard]] std::uint16_t NormalHalfMagnitude(
    int exponent, std::uint32_t mantissa) noexcept
{
    auto halfExponent = static_cast<std::uint32_t>(exponent + 15);
    auto halfMantissa = mantissa >> 13U;
    const auto remainder = mantissa & 0x1FFFU;
    if (RoundMantissaUp(remainder, 0x1000U, halfMantissa))
    {
        ++halfMantissa;
        if (halfMantissa == 0x400U)
        {
            halfMantissa = 0U;
            ++halfExponent;
            if (halfExponent >= 31U) return 0x7C00U;
        }
    }
    return static_cast<std::uint16_t>((halfExponent << 10U) | halfMantissa);
}

[[nodiscard]] std::uint16_t FloatToHalf(float value) noexcept
{
    // IEEE-754 round-to-nearest-even conversion, including signed gamut-fit
    // excursions. The generator rejects nonfinite or FP16-overflow outputs.
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127;
    const auto mantissa = bits & 0x007FFFFFU;

    if (exponent == 128)
        return static_cast<std::uint16_t>(
            sign | (mantissa == 0U ? 0x7C00U : 0x7E00U));
    if (exponent > 15)
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    if (exponent < -25) return sign;
    const auto magnitude = exponent < -14 ?
        SubnormalHalfMantissa(exponent, mantissa) :
        NormalHalfMagnitude(exponent, mantissa);
    return static_cast<std::uint16_t>(sign | magnitude);
}

struct PreparedOperator
{
    explicit PreparedOperator(const OperatorLutData& data) noexcept :
        algorithm(data.algorithm), lookStrength(data.parameters.acesLookStrength),
        aces2((algorithm == OperatorLutAlgorithm::aces2 || algorithm == OperatorLutAlgorithm::aces2Lmt) ?
            CreateAces2Transform(data.displayPeakNits) : Aces2TransformHandle{}),
        agx(algorithm == OperatorLutAlgorithm::agX ?
            CreateAgxTransform(data.displayPeakNits, data.parameters) :
            std::optional<AgxTransform>{}),
        gt7(algorithm == OperatorLutAlgorithm::gt7 ?
            CreateGt7Transform(data.displayPeakNits, data.parameters) : Gt7TransformHandle{})
    {}

    OperatorLutAlgorithm algorithm;
    double lookStrength;
    Aces2TransformHandle aces2;
    std::optional<AgxTransform> agx;
    Gt7TransformHandle gt7;

    [[nodiscard]] bool IsValid() const noexcept
    {
        switch (algorithm)
        {
        case OperatorLutAlgorithm::aces2:
        case OperatorLutAlgorithm::aces2Lmt: return aces2 != nullptr;
        case OperatorLutAlgorithm::agX: return agx.has_value();
        case OperatorLutAlgorithm::gt7: return gt7 != nullptr;
        }
        return false;
    }

    [[nodiscard]] LinearRgb Evaluate(LinearRgb sceneLinear) const noexcept
    {
        switch (algorithm)
        {
        case OperatorLutAlgorithm::aces2:
        case OperatorLutAlgorithm::aces2Lmt:
            return EvaluateAces(sceneLinear);
        case OperatorLutAlgorithm::agX:
            return agx.has_value() ? EvaluateAgxLutNits(*agx, sceneLinear) : LinearRgb{};
        case OperatorLutAlgorithm::gt7:
            return gt7 != nullptr ? EvaluateGt7LutNits(*gt7, sceneLinear) : LinearRgb{};
        }
        return {};
    }

    [[nodiscard]] LinearRgb EvaluateAces(LinearRgb sceneLinear) const noexcept
    {
        if (!aces2) return {};
        return algorithm == OperatorLutAlgorithm::aces2Lmt ?
            EvaluateAces2LmtLutNits(*aces2, sceneLinear, lookStrength) :
            EvaluateAces2LutNits(*aces2, sceneLinear);
    }
};

[[nodiscard]] bool IsAces2Algorithm(
    OperatorLutAlgorithm algorithm) noexcept
{
    return algorithm == OperatorLutAlgorithm::aces2 ||
        algorithm == OperatorLutAlgorithm::aces2Lmt;
}

[[nodiscard]] bool IsLutAlgorithm(
    OperatorLutAlgorithm algorithm) noexcept
{
    return algorithm == OperatorLutAlgorithm::aces2 ||
        algorithm == OperatorLutAlgorithm::aces2Lmt ||
        algorithm == OperatorLutAlgorithm::agX ||
        algorithm == OperatorLutAlgorithm::gt7;
}

[[nodiscard]] bool IsValidPeak(double peak) noexcept
{
    return std::isfinite(peak) && peak >= kMinimumDisplayPeakNits &&
        peak <= kMaximumDisplayPeakNits;
}

[[nodiscard]] bool IsFiniteRgb(LinearRgb value) noexcept
{
    return std::isfinite(value.red) && std::isfinite(value.green) &&
        std::isfinite(value.blue);
}

[[nodiscard]] bool FitsHalfChannels(LinearRgb value) noexcept
{
    return std::abs(value.red) <= kOperatorLutSceneMaximum &&
        std::abs(value.green) <= kOperatorLutSceneMaximum &&
        std::abs(value.blue) <= kOperatorLutSceneMaximum;
}

[[nodiscard]] double ShaperCenter(
    OperatorLutAlgorithm algorithm, double peak) noexcept
{
    if (IsAces2Algorithm(algorithm)) return 2.4;
    return algorithm == OperatorLutAlgorithm::gt7 ?
        std::log2(8.0 * peak / kSceneReferenceWhiteNits) : 6.0;
}

[[nodiscard]] double ShaperWidth(
    OperatorLutAlgorithm algorithm) noexcept
{
    return algorithm == OperatorLutAlgorithm::gt7 ? 2.5 : 3.0;
}

[[nodiscard]] double LogarithmicWeight(OperatorLutAlgorithm algorithm) noexcept
{
    return IsAces2Algorithm(algorithm) ? 0.25 :
        (algorithm == OperatorLutAlgorithm::agX ? 0.3 : 0.4);
}

struct ShaperBounds
{
    double minimum;
    double maximum;
};

[[nodiscard]] double ShapeLogarithm(double logarithm,
    OperatorLutAlgorithm algorithm, double peak,
    ShaperBounds bounds) noexcept
{
    const auto angle = std::atan((logarithm - ShaperCenter(algorithm, peak)) /
        ShaperWidth(algorithm));
    const auto normalizedAngle = (angle - bounds.minimum) / (bounds.maximum - bounds.minimum);
    const auto logSpan = std::log2(kOperatorLutSceneMaximum +
        OperatorLutInputOffset(algorithm)) + 16.0;
    return std::lerp(normalizedAngle, (logarithm + 16.0) / logSpan,
        LogarithmicWeight(algorithm));
}

[[nodiscard]] LinearRgb Add(LinearRgb left, LinearRgb right) noexcept
{
    return {left.red + right.red, left.green + right.green,
        left.blue + right.blue};
}

[[nodiscard]] LinearRgb Scale(LinearRgb value, double scale) noexcept
{
    return {value.red * scale, value.green * scale, value.blue * scale};
}

[[nodiscard]] LinearRgb Subtract(LinearRgb left, LinearRgb right) noexcept
{
    return Add(left, Scale(right, -1.0));
}

struct CoefficientWorkspace
{
    explicit CoefficientWorkspace(std::size_t count) :
        factors(count), inverseDiagonal(count), upper(count, 1.0),
        right(count), coefficients(count), padded(count + 2U)
    {
        upper.front() = 2.0;
        upper.back() = 0.0;
        double diagonal = 4.0;
        inverseDiagonal.front() = 1.0 / diagonal;
        for (std::size_t index = 1; index < count; ++index)
        {
            factors[index] = (index + 1U == count ? 2.0 : 1.0) / diagonal;
            diagonal = 4.0 - factors[index] * upper[index - 1U];
            inverseDiagonal[index] = 1.0 / diagonal;
        }
    }

    std::vector<double> factors;
    std::vector<double> inverseDiagonal;
    std::vector<double> upper;
    std::vector<LinearRgb> right;
    std::vector<LinearRgb> coefficients;
    std::vector<LinearRgb> padded;
};

// Direct separable solve for uniform interpolating cubic B-spline
// coefficients. The endpoint slopes use the four-point cubic formula; the
// returned line includes one coefficient ghost at each end.
[[nodiscard]] const std::vector<LinearRgb>& FilterCoefficientLine(
    const std::vector<LinearRgb>& samples, CoefficientWorkspace& work)
{
    const auto count = samples.size();
    auto& right = work.right;
    auto& coefficients = work.coefficients;
    auto& padded = work.padded;
    for (std::size_t index = 0; index < count; ++index)
        right[index] = Scale(samples[index], 6.0);

    const auto lowSlope = Scale(Add(
        Add(Scale(samples[0], -11.0), Scale(samples[1], 18.0)),
        Add(Scale(samples[2], -9.0), Scale(samples[3], 2.0))), 1.0 / 6.0);
    const auto highSlope = Scale(Add(
        Add(Scale(samples[count - 1U], 11.0),
            Scale(samples[count - 2U], -18.0)),
        Add(Scale(samples[count - 3U], 9.0),
            Scale(samples[count - 4U], -2.0))), 1.0 / 6.0);
    right.front() = Add(Scale(samples.front(), 6.0), Scale(lowSlope, 2.0));
    right.back() = Subtract(Scale(samples.back(), 6.0), Scale(highSlope, 2.0));

    for (std::size_t index = 1; index < count; ++index)
    {
        right[index] = Subtract(right[index],
            Scale(right[index - 1U], work.factors[index]));
    }
    coefficients.back() = Scale(right.back(), work.inverseDiagonal.back());
    for (std::size_t index = count - 1U; index-- > 0U;)
        coefficients[index] = Scale(Subtract(right[index],
            Scale(coefficients[index + 1U], work.upper[index])),
            work.inverseDiagonal[index]);

    std::copy(coefficients.begin(), coefficients.end(), padded.begin() + 1U);
    padded.front() = Subtract(coefficients[1U], Scale(lowSlope, 2.0));
    padded.back() = Add(coefficients[count - 2U], Scale(highSlope, 2.0));
    return padded;
}

[[nodiscard]] std::size_t CoefficientIndex(
    std::uint32_t x, std::uint32_t y, std::uint32_t z,
    const std::array<std::uint32_t, 3>& size) noexcept
{
    return (static_cast<std::size_t>(z) * size[1] + y) * size[0] + x;
}

struct CoefficientAxisFilter
{
    const std::vector<LinearRgb>& samples;
    std::vector<LinearRgb>& next;
    const std::array<std::uint32_t, 3>& size;
    const std::array<std::uint32_t, 3>& nextSize;
    std::size_t axis;
    std::vector<LinearRgb>& line;
    CoefficientWorkspace& work;

    void FilterLine(std::array<std::uint32_t, 3> point) const
    {
        for (std::uint32_t index = 0; index < size[axis]; ++index)
        {
            point[axis] = index;
            line[index] = samples[CoefficientIndex(
                point[0], point[1], point[2], size)];
        }
        const auto& padded = FilterCoefficientLine(line, work);
        for (std::uint32_t index = 0; index < nextSize[axis]; ++index)
        {
            point[axis] = index;
            next[CoefficientIndex(point[0], point[1], point[2],
                nextSize)] = padded[index];
        }
    }

    [[nodiscard]] bool Apply(OperatorLutCancellation cancellation) const
    {
        auto loopSize = size;
        loopSize[axis] = 1U;
        for (std::uint32_t z = 0; z < loopSize[2]; ++z)
        {
            for (std::uint32_t y = 0; y < loopSize[1]; ++y)
            {
                if (cancellation.IsCancelled()) return false;
                for (std::uint32_t x = 0; x < loopSize[0]; ++x)
                {
                    FilterLine({x, y, z});
                }
            }
        }
        return true;
    }
};

[[nodiscard]] std::vector<LinearRgb> PrefilterCoefficients(
    std::vector<LinearRgb> samples, std::uint32_t logicalEdge,
    OperatorLutCancellation cancellation = {})
{
    CoefficientWorkspace work(logicalEdge);
    std::vector<LinearRgb> line(logicalEdge);
    std::array<std::uint32_t, 3> size{logicalEdge, logicalEdge, logicalEdge};
    for (std::size_t axis = 0; axis < 3U; ++axis)
    {
        if (cancellation.IsCancelled()) return {};
        auto nextSize = size;
        nextSize[axis] += kOperatorLutCoefficientPadding;
        std::vector<LinearRgb> next(static_cast<std::size_t>(nextSize[0]) *
            nextSize[1] * nextSize[2]);
        const CoefficientAxisFilter filter{samples, next, size, nextSize,
            axis, line, work};
        if (!filter.Apply(cancellation)) return {};
        samples = std::move(next);
        size = nextSize;
    }
    return samples;
}

struct LatticeSampler
{
    std::vector<LinearRgb>& nodes;
    const PreparedOperator& prepared;
    const std::vector<double>& coordinates;
    OperatorLutCancellation cancellation;
    std::size_t nodeIndex{};

    [[nodiscard]] bool SampleRow(double green, double blue) noexcept
    {
        for (const auto red : coordinates)
        {
            const auto mapped = prepared.Evaluate({red, green, blue});
            if (!IsFiniteRgb(mapped) || !FitsHalfChannels(mapped)) return false;
            nodes[nodeIndex++] = mapped;
        }
        return true;
    }

    [[nodiscard]] bool Sample() noexcept
    {
        for (const auto blue : coordinates)
            for (const auto green : coordinates)
            {
                if (cancellation.IsCancelled()) return false;
                if (!SampleRow(green, blue)) return false;
            }
        return true;
    }
};

[[nodiscard]] std::vector<double> BuildCoordinates(const OperatorLutData& data)
{
    const auto denominator = static_cast<double>(data.edge - 1U);
    std::vector<double> coordinates(data.edge);
    for (std::uint32_t index = 0; index < data.edge; ++index)
        coordinates[index] = DecodeOperatorLutCoordinate(
            index / denominator, data.algorithm, data.displayPeakNits);
    return coordinates;
}

[[nodiscard]] bool IsValidBuildRequest(OperatorLutAlgorithm algorithm,
    double peak) noexcept
{
    return IsLutAlgorithm(algorithm) && IsValidPeak(peak);
}

[[nodiscard]] bool EncodeCoefficients(const std::vector<LinearRgb>& nodes,
    OperatorLutData& result, OperatorLutCancellation cancellation)
{
    constexpr std::size_t channels{4};
    result.textureEdge = result.edge + kOperatorLutCoefficientPadding;
    result.rgba16.reserve(nodes.size() * channels);
    for (const auto value : nodes)
    {
        if ((result.rgba16.size() % (result.textureEdge * channels)) == 0U &&
            cancellation.IsCancelled()) return false;
        if (!IsFiniteRgb(value) ||
            std::max({std::abs(value.red), std::abs(value.green),
                std::abs(value.blue)}) > kOperatorLutSceneMaximum)
            return false;
        result.rgba16.push_back(FloatToHalf(static_cast<float>(value.red)));
        result.rgba16.push_back(FloatToHalf(static_cast<float>(value.green)));
        result.rgba16.push_back(FloatToHalf(static_cast<float>(value.blue)));
        result.rgba16.push_back(FloatToHalf(1.0F));
    }
    return true;
}

[[nodiscard]] bool BakeOperatorLut(
    OperatorLutData& result, OperatorLutCancellation cancellation)
{
    const auto nodeCount = static_cast<std::size_t>(result.edge) *
        result.edge * result.edge;
    std::vector<LinearRgb> nodes;
    nodes.resize(nodeCount);
    const PreparedOperator prepared(result);
    if (!prepared.IsValid()) return false;
    const auto coordinates = BuildCoordinates(result);
    LatticeSampler sampler{nodes, prepared, coordinates, cancellation};
    if (!sampler.Sample()) return false;
    nodes = PrefilterCoefficients(std::move(nodes), result.edge, cancellation);
    if (nodes.empty()) return false;
    if (!EncodeCoefficients(nodes, result, cancellation)) return false;
    return true;
}

double OperatorLutInputOffset(OperatorLutAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
    case OperatorLutAlgorithm::aces2:
    case OperatorLutAlgorithm::aces2Lmt:
    case OperatorLutAlgorithm::agX:
    case OperatorLutAlgorithm::gt7: return 1.0 / 65536.0;
    }
    return 0.0;
}

std::uint32_t OperatorLutEdge(
    OperatorLutAlgorithm algorithm) noexcept
{
    if (!IsLutAlgorithm(algorithm)) return 0U;
    return IsAces2Algorithm(algorithm) ? kAces2OperatorLutEdge :
        (algorithm == OperatorLutAlgorithm::agX ?
            kAgxOperatorLutEdge : kGt7OperatorLutEdge);
}

double OperatorLutInputShaperMinimum(
    OperatorLutAlgorithm algorithm,
    double displayPeakNits) noexcept
{
    if (!IsLutAlgorithm(algorithm) || !IsValidPeak(displayPeakNits))
        return 0.0;
    const double minimum = std::log2(OperatorLutInputOffset(algorithm));
    return std::atan((minimum - ShaperCenter(algorithm, displayPeakNits)) /
        ShaperWidth(algorithm));
}

double OperatorLutInputShaperMaximum(
    OperatorLutAlgorithm algorithm,
    double displayPeakNits) noexcept
{
    if (!IsLutAlgorithm(algorithm) || !IsValidPeak(displayPeakNits))
        return 0.0;
    const double maximum = kOperatorLutSceneMaximum;
    const double logarithm = std::log2(maximum + OperatorLutInputOffset(algorithm));
    return std::atan((logarithm - ShaperCenter(algorithm, displayPeakNits)) /
        ShaperWidth(algorithm));
}

double DecodeOperatorLutCoordinate(
    double coordinate,
    OperatorLutAlgorithm algorithm,
    double displayPeakNits) noexcept
{
    if (!std::isfinite(coordinate)) return 0.0;
    coordinate = std::clamp(coordinate, 0.0, 1.0);
    const auto minimum =
        OperatorLutInputShaperMinimum(algorithm, displayPeakNits);
    const auto maximum =
        OperatorLutInputShaperMaximum(algorithm, displayPeakNits);
    if (!(maximum > minimum)) return 0.0;
    if (coordinate == 0.0) return 0.0;
    if (coordinate == 1.0) return kOperatorLutSceneMaximum;
    double low = -16.0;
    double high = std::log2(kOperatorLutSceneMaximum + OperatorLutInputOffset(algorithm));
    // Inversion runs only once per axis knot during a bake. The forward map
    // is smooth over the whole domain, avoiding a dark-cell splice.
    for (unsigned iteration = 0; iteration < 48; ++iteration)
    {
        const auto middle = (low + high) * 0.5;
        if (ShapeLogarithm(middle, algorithm, displayPeakNits, {minimum, maximum}) < coordinate)
            low = middle;
        else
            high = middle;
    }
    return std::max(0.0, std::exp2((low + high) * 0.5) - OperatorLutInputOffset(algorithm));
}

}  // namespace

std::array<float, 4> OperatorLutShaperParameters(
    OperatorLutAlgorithm algorithm, double displayPeakNits) noexcept
{
    const auto minimum = OperatorLutInputShaperMinimum(algorithm, displayPeakNits);
    const auto maximum = OperatorLutInputShaperMaximum(algorithm, displayPeakNits);
    if (!(maximum > minimum)) return {};
    const auto weight = LogarithmicWeight(algorithm);
    const auto angleGain = (1.0 - weight) / (maximum - minimum);
    const auto logGain = weight / (std::log2(kOperatorLutSceneMaximum +
        OperatorLutInputOffset(algorithm)) + 16.0);
    return {static_cast<float>(angleGain),
        static_cast<float>(-minimum * angleGain + 16.0 * logGain),
        static_cast<float>(ShaperCenter(algorithm, displayPeakNits)),
        static_cast<float>(logGain)};
}

bool OperatorLutData::IsValid() const noexcept
{
    if (!IsLutAlgorithm(algorithm) || !IsValidPeak(displayPeakNits)) return false;
    if (!ValidateToneMapperParameters(algorithm, parameters) ||
        parameters != ActiveToneMapperParameters(algorithm, parameters)) return false;
    if (edge != OperatorLutEdge(algorithm) ||
        textureEdge != edge + kOperatorLutCoefficientPadding) return false;
    constexpr std::size_t channels{4};
    const auto expected = static_cast<std::size_t>(textureEdge) * textureEdge * textureEdge *
        channels;
    return rgba16.size() == expected;
}

OperatorLutData BuildOperatorLut(
    OperatorLutAlgorithm algorithm,
    double displayPeakNits,
    OperatorLutCancellation cancellation) noexcept
{
    return BuildOperatorLut(algorithm, displayPeakNits, ToneMapperParameters{}, cancellation);
}

OperatorLutData BuildOperatorLut(
    OperatorLutAlgorithm algorithm,
    double displayPeakNits,
    const ToneMapperParameters& parameters,
    OperatorLutCancellation cancellation) noexcept
{
    if (cancellation.IsCancelled() || !IsValidBuildRequest(algorithm, displayPeakNits)) return {};
    if (!ValidateToneMapperParameters(algorithm, parameters)) return {};
    OperatorLutData result;
    result.edge = OperatorLutEdge(algorithm);
    result.displayPeakNits = displayPeakNits;
    result.algorithm = algorithm;
    result.parameters = ActiveToneMapperParameters(algorithm, parameters);
    try
    {
        if (!BakeOperatorLut(result, cancellation)) return {};
    }
    catch (...)
    {
        return {};
    }
    if (cancellation.IsCancelled()) return {};
    return result.IsValid() ? std::move(result) : OperatorLutData{};
}

}  // namespace fo4hdr
