// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "OperatorLutTestSupport.hpp"

#include "FO4Isotope/tonemap/Aces2.hpp"
#include "FO4Isotope/tonemap/Agx.hpp"
#include "FO4Isotope/tonemap/Gt7.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace fo4hdr::test {

[[nodiscard]] double HalfToDouble(std::uint16_t bits) noexcept
{
    const auto negative = (bits & 0x8000U) != 0U;
    const auto exponent = (bits >> 10U) & 0x1FU;
    const auto mantissa = bits & 0x3FFU;
    double value{};
    if (exponent == 0U)
        value = std::ldexp(static_cast<double>(mantissa), -24);
    else if (exponent == 0x1FU)
        value = mantissa == 0U ?
            std::numeric_limits<double>::infinity() :
            std::numeric_limits<double>::quiet_NaN();
    else
        value = std::ldexp(
            1.0 + static_cast<double>(mantissa) / 1024.0,
            static_cast<int>(exponent) - 15);
    return negative ? -value : value;
}

// Independently sum all 64 coefficient-lattice contributions from the actual
// half-precision texture rather than sharing the shader's grouped fetches.
[[nodiscard]] LinearRgb SampleOperatorLut(
    const OperatorLutData& lut, LinearRgb scene) noexcept
{
    const auto coordinate = [&lut](double value) noexcept {
        const auto shaped = EncodeOperatorLutCoordinate(
            value, lut.algorithm, lut.displayPeakNits);
        return shaped * static_cast<double>(lut.edge - 1U);
    };
    const std::array position{
        coordinate(scene.red), coordinate(scene.green),
        coordinate(scene.blue)};
    // The production exact-black bypass applies after input sanitization.
    if (position == std::array{0.0, 0.0, 0.0}) return {};
    std::array<std::uint32_t, 3> base{};
    std::array<std::array<double, 4>, 3> weights{};
    for (std::size_t axis = 0; axis < 3U; ++axis)
    {
        base[axis] = std::min(
            static_cast<std::uint32_t>(std::floor(position[axis])),
            lut.edge - 2U);
        const auto t = position[axis] - base[axis];
        const auto t2 = t * t;
        const auto t3 = t2 * t;
        const auto oneMinusT = 1.0 - t;
        weights[axis] = {
            oneMinusT * oneMinusT * oneMinusT / 6.0,
            (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0,
            (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0,
            t3 / 6.0};
    }
    const auto fetch = [&lut](std::uint32_t x, std::uint32_t y,
                           std::uint32_t z, std::size_t channel) noexcept {
        const auto texel = (static_cast<std::size_t>(z) * lut.textureEdge *
            lut.textureEdge + static_cast<std::size_t>(y) *
            lut.textureEdge + x) * 4U;
        return HalfToDouble(lut.rgba16[texel + channel]);
    };
    std::array<double, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel)
    {
        for (std::uint32_t coefficient = 0; coefficient < 64U; ++coefficient)
        {
            const auto x = coefficient % 4U;
            const auto y = (coefficient / 4U) % 4U;
            const auto z = coefficient / 16U;
            result[channel] += weights[0][x] * weights[1][y] *
                weights[2][z] * fetch(base[0] + x,
                    base[1] + y, base[2] + z, channel);
        }
    }
    const LinearRgb mapped{result[0], result[1], result[2]};
    if (lut.algorithm == ProductionToneMapperAlgorithm::agX)
        return ApplyAgxDisplayGamut(mapped, lut.displayPeakNits);
    return ContainBt2020Nits(mapped, lut.displayPeakNits);
}

namespace {

[[nodiscard]] bool IsFiniteRgb(LinearRgb value) noexcept
{
    return std::isfinite(value.red) && std::isfinite(value.green) &&
        std::isfinite(value.blue);
}

[[nodiscard]] bool IsExactBlack(LinearRgb value) noexcept
{
    return value.red == 0.0 && value.green == 0.0 && value.blue == 0.0;
}

[[nodiscard]] double DecodeHalf(std::uint16_t value) noexcept
{
    const auto exponent = (value >> 10U) & 31U;
    const auto mantissa = value & 1023U;
    if (exponent == 31U) return std::numeric_limits<double>::quiet_NaN();
    const auto magnitude = exponent == 0U ? std::ldexp(mantissa, -24) :
        std::ldexp(1024U + mantissa, static_cast<int>(exponent) - 25);
    return (value & 0x8000U) != 0U ? -magnitude : magnitude;
}

struct SampleAxis
{
    std::uint32_t base{};
    std::array<double, 4> weight{};
};

[[nodiscard]] SampleAxis PrepareAxis(const OperatorLutData& lut, double value) noexcept
{
    const auto position = EncodeOperatorLutCoordinate(value, lut.algorithm,
        lut.displayPeakNits) * static_cast<double>(lut.edge - 1U);
    const auto base = std::min(static_cast<std::uint32_t>(std::floor(position)),
        lut.edge - 2U);
    const auto t = position - base;
    const auto t2 = t * t;
    const auto t3 = t2 * t;
    const auto inverse = 1.0 - t;
    return {base, {inverse * inverse * inverse / 6.0,
        (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0,
        (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0,
        t3 / 6.0}};
}

[[nodiscard]] LinearRgb ReconstructOperatorLut(
    const OperatorLutData& lut, LinearRgb scene) noexcept
{
    // Match the shader's exact-black endpoint without flattening colored shadows.
    if (IsExactBlack(scene)) return {};
    const auto red = PrepareAxis(lut, scene.red);
    const auto green = PrepareAxis(lut, scene.green);
    const auto blue = PrepareAxis(lut, scene.blue);
    LinearRgb mapped{};
    for (std::size_t z = 0; z < 4U; ++z)
        for (std::size_t y = 0; y < 4U; ++y)
            for (std::size_t x = 0; x < 4U; ++x)
            {
                const auto texel = (((blue.base + z) * lut.textureEdge +
                    green.base + y) * lut.textureEdge + red.base + x) * 4U;
                const auto weight = red.weight[x] * green.weight[y] * blue.weight[z];
                mapped.red += weight * DecodeHalf(lut.rgba16[texel]);
                mapped.green += weight * DecodeHalf(lut.rgba16[texel + 1U]);
                mapped.blue += weight * DecodeHalf(lut.rgba16[texel + 2U]);
            }
    // Presentation containment sanitizes nonfinite inputs, so reject them first.
    if (!IsFiniteRgb(mapped)) return mapped;
    return lut.algorithm == ProductionToneMapperAlgorithm::agX ?
        ApplyAgxDisplayGamut(mapped, lut.displayPeakNits) :
        ContainBt2020Nits(mapped, lut.displayPeakNits);
}

struct QualityReference
{
    explicit QualityReference(const OperatorLutData& lut) noexcept :
        data(lut),
        aces2(data.algorithm == ProductionToneMapperAlgorithm::aces2 ?
            CreateAces2Transform(data.displayPeakNits) : Aces2TransformHandle{}),
        agx(data.algorithm == ProductionToneMapperAlgorithm::agX ?
            CreateAgxTransform(data.displayPeakNits, data.toneMapperParameters) :
            std::optional<AgxTransform>{}),
        gt7(data.algorithm == ProductionToneMapperAlgorithm::gt7 ?
            CreateGt7Transform(data.displayPeakNits, data.toneMapperParameters) :
            Gt7TransformHandle{})
    {}

    const OperatorLutData& data;
    Aces2TransformHandle aces2;
    std::optional<AgxTransform> agx;
    Gt7TransformHandle gt7;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return aces2 != nullptr || agx.has_value() || gt7 != nullptr;
    }

    [[nodiscard]] LinearRgb Map(LinearRgb scene) const noexcept
    {
        if (aces2 != nullptr)
            return MapAdaptedAces2ToBt2020Nits(*aces2, scene,
                data.toneMapperParameters.acesLookStrength);
        if (agx.has_value()) return MapAgxToBt2020Nits(*agx, scene);
        return ContainBt2020Nits(EvaluateGt7LutNits(*gt7, scene),
            data.displayPeakNits);
    }
};

[[nodiscard]] double NearestRankP99(std::vector<double>& errors)
{
    // ceil(0.99*N)-1 without floating-point rounding or an integer product.
    const auto rank = errors.size() - errors.size() / 100U - 1U;
    const auto selected = errors.begin() + static_cast<std::ptrdiff_t>(rank);
    std::nth_element(errors.begin(), selected, errors.end());
    return *selected;
}

[[nodiscard]] bool IsValidError(double error) noexcept
{
    return std::isfinite(error) && error >= 0.0;
}

[[nodiscard]] bool IsValidQuality(const OperatorLutQuality& quality) noexcept
{
    return quality.available && quality.sampleCount != 0U &&
        IsValidError(quality.maximumDeltaEItp) && IsValidError(quality.broadP99DeltaEItp) &&
        IsValidError(quality.holdoutP99DeltaEItp);
}

struct QualityEvaluator
{
    const QualityReference& reference;
    OperatorLutCancellation cancellation;
    OperatorLutQualityMeasurements measurements;
    std::size_t count{};

    [[nodiscard]] bool Measure(LinearRgb scene, OperatorLutQualityRegion region)
    {
        if ((count++ % 128U) == 0U && cancellation.IsCancelled()) return false;
        const auto error = DeltaEItpBt2020Nits(reference.Map(scene),
            ReconstructOperatorLut(reference.data, scene));
        measurements.Record(error, region);
        return std::isfinite(error);
    }

    [[nodiscard]] bool MeasureCorpus(std::span<const LinearRgb> corpus,
        OperatorLutQualityRegion region)
    {
        for (const auto scene : corpus)
            if (!Measure(scene, region)) return false;
        return true;
    }

    [[nodiscard]] bool MeasureNeutrals()
    {
        constexpr std::size_t countNeutrals{4096};
        const auto maximumEv = std::log2(kOperatorLutSceneMaximum);
        for (std::size_t sample = 0; sample < countNeutrals; ++sample)
        {
            const auto fraction = static_cast<double>(sample) /
                static_cast<double>(countNeutrals - 1U);
            const auto value = static_cast<double>(static_cast<float>(
                std::exp2(-24.0 + (maximumEv + 24.0) * fraction)));
            if (!Measure({value, value, value}, OperatorLutQualityRegion::other))
                return false;
        }
        return true;
    }

    [[nodiscard]] bool MeasureAll()
    {
        if (!MeasureCorpus(BuildOperatorLutCorpus(reference.data.displayPeakNits),
                OperatorLutQualityRegion::broad)) return false;
        if (!MeasureCorpus(BuildOperatorLutHoldoutCorpus(),
                OperatorLutQualityRegion::holdout)) return false;
        if (!MeasureCorpus(BuildNearBlackOperatorCorpus(),
                OperatorLutQualityRegion::other)) return false;
        return MeasureNeutrals();
    }
};

} // namespace

double OperatorLutMaximumDeltaEItp(ProductionToneMapperAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
    case ProductionToneMapperAlgorithm::aces2: return 4.8;
    case ProductionToneMapperAlgorithm::agX:
    case ProductionToneMapperAlgorithm::gt7: return 2.4;
    }
    return 0.0;
}

bool OperatorLutQuality::MeetsPracticalLimit(ProductionToneMapperAlgorithm algorithm) const noexcept
{
    const auto nominalMaximum = OperatorLutMaximumDeltaEItp(algorithm);
    if (nominalMaximum == 0.0) return false;
    // Compare raw maxima to the first positive rounding tie, avoiding double rounding.
    const auto cutoff = (nominalMaximum * 10.0 + 0.5) / 10.0;
    return IsValidQuality(*this) && maximumDeltaEItp < cutoff &&
        broadP99DeltaEItp <= kOperatorLutPercentileDeltaEItp &&
        holdoutP99DeltaEItp <= kOperatorLutPercentileDeltaEItp;
}

bool OperatorLutQuality::MeetsFidelityGoal(ProductionToneMapperAlgorithm algorithm) const noexcept
{
    return MeetsPracticalLimit(algorithm) && maximumDeltaEItp <= kOperatorLutGoalDeltaEItp;
}

void OperatorLutQualityMeasurements::Record(double error, OperatorLutQualityRegion region)
{
    if (!IsValidError(error))
    {
        invalid_ = true;
        return;
    }
    if (sampleCount_ == std::numeric_limits<std::uint32_t>::max())
    {
        invalid_ = true;
        return;
    }
    ++sampleCount_;
    maximum_ = std::max(maximum_, error);
    if (region == OperatorLutQualityRegion::broad) broad_.push_back(error);
    if (region == OperatorLutQualityRegion::holdout) holdout_.push_back(error);
}

OperatorLutQuality OperatorLutQualityMeasurements::Summary()
{
    if (invalid_) return {};
    if (broad_.empty()) return {};
    if (holdout_.empty()) return {};
    return {true, maximum_, NearestRankP99(broad_), NearestRankP99(holdout_), sampleCount_};
}

OperatorLutQuality EvaluateOperatorLutQuality(
    const OperatorLutData& lut, OperatorLutCancellation cancellation) noexcept
{
    if (cancellation.IsCancelled() || !lut.IsValid()) return {};
    try
    {
        const QualityReference reference(lut);
        if (!reference.IsValid()) return {};
        QualityEvaluator evaluator{reference, cancellation, {}, 0U};
        if (!evaluator.MeasureAll()) return {};
        if (cancellation.IsCancelled()) return {};
        return evaluator.measurements.Summary();
    }
    catch (...)
    {
        return {};
    }
}

[[nodiscard]] std::vector<LinearRgb> BuildNearBlackOperatorCorpus()
{
    std::vector<LinearRgb> result;
    result.reserve(8192);
    constexpr std::array directions{LinearRgb{1, 1, 1}, LinearRgb{1, 0, 0},
        LinearRgb{0, 1, 0}, LinearRgb{0, 0, 1}, LinearRgb{1, 0.25, 0.0625},
        LinearRgb{0.0625, 1, 0.25}, LinearRgb{0.25, 0.0625, 1},
        LinearRgb{1, 0, 1}};
    // Positive logarithmic samples expose dark interpolation error without
    // the exact-black endpoint or bright samples hiding it in a global mean.
    for (unsigned step = 0; step < 1024; ++step)
    {
        const auto level = std::exp2(-24.0 + 20.0 * step / 1023.0);
        for (const auto direction : directions)
            result.push_back({static_cast<float>(level * direction.red),
                static_cast<float>(level * direction.green),
                static_cast<float>(level * direction.blue)});
    }
    return result;
}

// Deterministic held-out colors use a distinct random sequence and generation
// patterns from BuildOperatorLutCorpus. Keep this independent of LUT knots and
// shapers so representation changes cannot tune against the validation set.
[[nodiscard]] std::vector<LinearRgb> BuildOperatorLutHoldoutCorpus()
{
    constexpr std::size_t independentCount{16384};
    constexpr std::size_t hueCount{8192};
    constexpr std::size_t faceCount{8192};
    constexpr double minimumEv{-24.0};
    const auto exposureRangeEv = std::log2(65504.0) - minimumEv;
    std::vector<LinearRgb> result;
    result.reserve(independentCount + hueCount + faceCount);
    const auto add = [&result](double r, double g, double b) {
        result.push_back({static_cast<float>(r), static_cast<float>(g),
            static_cast<float>(b)});
    };
    std::uint32_t seed{0xC93A7F15U};
    const auto random = [&seed] {
        seed = seed * 747796405U + 2891336453U;
        auto word = ((seed >> ((seed >> 28U) + 4U)) ^ seed) * 277803737U;
        word = (word >> 22U) ^ word;
        return static_cast<double>(word >> 8U) / 16777216.0;
    };
    const auto logValue = [&random, exposureRangeEv] {
        return std::exp2(minimumEv + exposureRangeEv * random());
    };

    for (std::size_t sample = 0; sample < independentCount; ++sample)
    {
        const auto red = logValue();
        const auto green = logValue();
        const auto blue = logValue();
        add(red, green, blue);
    }

    // A single exposure scales each colored ray. Varying saturation keeps this
    // distinct from the exact zero-channel faces below.
    for (std::size_t sample = 0; sample < hueCount; ++sample)
    {
        const auto hue = 6.0 * random();
        const auto x = 1.0 - std::abs(std::fmod(hue, 2.0) - 1.0);
        const std::array colors{LinearRgb{1, x, 0}, LinearRgb{x, 1, 0},
            LinearRgb{0, 1, x}, LinearRgb{0, x, 1},
            LinearRgb{x, 0, 1}, LinearRgb{1, 0, x}};
        const auto hueColor = colors[std::min(
            static_cast<std::size_t>(hue), colors.size() - 1U)];
        const auto saturation = 0.25 + 0.74 * random();
        const auto exposure = logValue();
        add(exposure * (1.0 - saturation + saturation * hueColor.red),
            exposure * (1.0 - saturation + saturation * hueColor.green),
            exposure * (1.0 - saturation + saturation * hueColor.blue));
    }

    for (std::size_t sample = 0; sample < faceCount; ++sample)
    {
        std::array channels{logValue(), logValue(), logValue()};
        channels[sample % channels.size()] = 0.0;
        add(channels[0], channels[1], channels[2]);
    }
    return result;
}

namespace {

template <class AddColor>
void AppendOperatorHueSweep(double peak, const AddColor& add)
{
    // Dense saturated hue sweeps cover exposure and operator-domain boundaries.
    const std::array intensities{0.001, 0.01, 0.18, 1.0, 4.0, 5.638,
        8.0, 16.0, 16.291740, 20.0, 32.0, 39.217, 64.0, 128.0,
        512.0, 2048.0, 16384.0, 65504.0, 9.0 * peak / 203.0};
    for (const auto intensity : intensities)
        for (unsigned hue = 0; hue < 360; ++hue)
        {
            const auto h = static_cast<double>(hue) / 60.0;
            const auto x = 1.0 - std::abs(std::fmod(h, 2.0) - 1.0);
            const std::array colors{LinearRgb{1, x, 0}, LinearRgb{x, 1, 0},
                LinearRgb{0, 1, x}, LinearRgb{0, x, 1},
                LinearRgb{x, 0, 1}, LinearRgb{1, 0, x}};
            const auto rgb = colors[static_cast<unsigned>(h)];
            add(rgb.red * intensity, rgb.green * intensity,
                rgb.blue * intensity);
        }
}

template <class AddColor>
void AppendOperatorCorpusAnchors(const AddColor& add)
{
    // These vectors expose premature per-channel clipping and saturated transitions.
    add(64, 0, 57.6);
    add(32, 2, 0.1);
    add(16, 2, 0.1);
    add(0.0056, 0.0665, 5.785);
    add(0.000286, 0.000044, 39.217);
    // Keep explicit anchors even where they overlap the sweeps so adjusting
    // sweep spacing cannot remove their coverage. CPU and GPU use the same inputs.
    constexpr std::array legacy{
        LinearRgb{0.0, 0.0, 0.0},
        LinearRgb{0.000001, 0.000001, 0.000001},
        LinearRgb{0.00001, 0.00001, 0.00001},
        LinearRgb{0.0001, 0.0001, 0.0001},
        LinearRgb{0.001, 0.001, 0.001},
        LinearRgb{0.01, 0.01, 0.01},
        LinearRgb{0.1, 0.1, 0.1},
        LinearRgb{0.18, 0.18, 0.18},
        LinearRgb{0.5, 0.5, 0.5},
        LinearRgb{0.97784, 0.97784, 0.97784},
        LinearRgb{1.0, 1.0, 1.0},
        LinearRgb{2.0, 2.0, 2.0},
        LinearRgb{4.0, 4.0, 4.0},
        LinearRgb{8.0, 8.0, 8.0},
        LinearRgb{16.0, 16.0, 16.0},
        LinearRgb{0.18, 0.0, 0.0},
        LinearRgb{0.0, 0.18, 0.0},
        LinearRgb{0.0, 0.0, 0.18},
        LinearRgb{0.18, 0.18, 0.0},
        LinearRgb{0.0, 0.18, 0.18},
        LinearRgb{0.18, 0.0, 0.18},
        LinearRgb{1.0, 0.0, 0.0},
        LinearRgb{0.0, 1.0, 0.0},
        LinearRgb{0.0, 0.0, 1.0},
        LinearRgb{1.0, 1.0, 0.0},
        LinearRgb{0.0, 1.0, 1.0},
        LinearRgb{1.0, 0.0, 1.0},
        LinearRgb{8.0, 0.0, 0.0},
        LinearRgb{0.0, 8.0, 0.0},
        LinearRgb{0.0, 0.0, 8.0},
        LinearRgb{8.0, 8.0, 0.0},
        LinearRgb{0.0, 8.0, 8.0},
        LinearRgb{8.0, 0.0, 8.0},
        LinearRgb{4.0, 2.0, 0.0},
        LinearRgb{2.0, 4.0, 0.0},
        LinearRgb{0.0, 4.0, 2.0},
        LinearRgb{0.0, 2.0, 4.0},
        LinearRgb{2.0, 0.0, 4.0},
        LinearRgb{4.0, 0.0, 2.0},
        LinearRgb{4.0, 1.0, 0.25},
        LinearRgb{2.03125, 0.6875, 0.53125},
        LinearRgb{16.0, 2.0, 0.1}
    };
    for (const auto color : legacy) add(color.red, color.green, color.blue);
}

} // namespace

// Shared CPU/GPU coverage, deliberately independent of LUT knots and shapers.
// Binary32 inputs make CPU and GPU compare the same color. This is an
// interpolation regression corpus, not a replacement for upstream oracles.
[[nodiscard]] std::vector<LinearRgb> BuildOperatorLutCorpus(double peak)
{
    std::vector<LinearRgb> result;
    result.reserve(40000);
    const auto add = [&result](double r, double g, double b) {
        result.push_back({static_cast<float>(r), static_cast<float>(g),
            static_cast<float>(b)});
    };
    constexpr std::array levels{0.0, 0.0001, 0.001, 0.01, 0.18, 1.0,
        4.0, 16.0, 20.0, 32.0, 64.0, 256.0, 4096.0, 65504.0};
    for (const auto r : levels)
        for (const auto g : levels)
            for (const auto b : levels) add(r, g, b);

    AppendOperatorHueSweep(peak, add);

    std::uint32_t seed{0x71A5C3E9U};
    const auto random = [&seed] {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<double>(seed >> 8U) / 16777216.0;
    };
    // Sample channels logarithmically from 2^-16 to each maximum. Separate
    // ordinary-scene and highlight sweeps keep high values from diluting
    // ordinary-scene coverage; explicit near-black anchors follow below.
    for (const auto maximum : {16.0, 65504.0})
        for (unsigned i = 0; i < 12000; ++i)
        {
            const auto channel = [&] {
                return std::exp2(-16.0 + (std::log2(maximum) + 16.0) * random());
            };
            const auto r = channel();
            const auto g = channel();
            const auto b = channel();
            add(r, g, b);
        }
    AppendOperatorCorpusAnchors(add);
    return result;
}

} // namespace fo4hdr::test
