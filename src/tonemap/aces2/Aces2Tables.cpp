// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
// Upstream: aces-aswf/aces-core@ee380496a201fd008841ff9399b29241bcd27a3a/lib/Lib.Academy.OutputTransform.ctl
// Upstream: aces-aswf/aces-output@90cadee61547c006ba8fad320ffacc171095bfad/d65/rec2100/Output.Academy.Rec2100-D65_1000nit_in_Rec2100-D65_ST2084.ctl
//
// Adapted from the ACES 2.0 forward Output Transform at:
// aces-aswf/aces-core@ee380496a201fd008841ff9399b29241bcd27a3a
//   lib/Lib.Academy.Utilities.ctl
//   lib/Lib.Academy.ColorSpaces.ctl
//   lib/Lib.Academy.Tonescale.ctl
//   lib/Lib.Academy.OutputTransform.ctl
// and the Rec.2100/D65 1000-nit parameterization at:
// aces-aswf/aces-output@90cadee61547c006ba8fad320ffacc171095bfad
//   d65/rec2100/Output.Academy.Rec2100-D65_1000nit_in_Rec2100-D65_ST2084.ctl
//
// Modified by FO4Isotope: parameterize the same Rec.2100 limiting gamut for
// 400-2000 nits and adapt dimensionless scene-linear BT.709/D65 to AP0/D60
// without changing neutral scene values, and stop at absolute display-linear
// Rec.2020. The direct AP0 forward boundary supports the pinned Contrast LMT;
// the shared forward path retains the official AP1 formation clamp.
// ACES2LMT alone blends a recomputed zero-toe shadow response into the scalar
// tone scale below internal 0.18 and uses a fixed-width gamut shoulder to avoid
// the reference reach/gamut crossing discontinuity.
// The official display-encoding/PQ stage is intentionally omitted because the
// renderer owns the single final ST 2084 boundary. Normalized upper-hull hue
// interpolation and a full bounded interval search correct the pinned CTL's
// hue interpolation/search issues. See THIRD_PARTY_NOTICES.md.

#include "Aces2Model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fo4hdr::aces2 {

[[nodiscard]] Vec3 GenerateCubeCorner(std::size_t corner) noexcept
{
    return {
        ((corner + 1) % kCuspCornerCount) < 3 ? 1.0 : 0.0,
        ((corner + 5) % kCuspCornerCount) < 3 ? 1.0 : 0.0,
        ((corner + 3) % kCuspCornerCount) < 3 ? 1.0 : 0.0
    };
}

void BuildLimitingCorners(
    std::array<Vec3, kTotalCornerCount>& rgbCorners,
    std::array<Vec3, kTotalCornerCount>& jmhCorners,
    const JMhParams& params,
    double peak) noexcept
{
    std::array<Vec3, kCuspCornerCount> temporaryRgb{};
    std::array<Vec3, kCuspCornerCount> temporaryJMh{};
    std::size_t minimum = 0;
    for (std::size_t i = 0; i < kCuspCornerCount; ++i)
    {
        const Vec3 unit = GenerateCubeCorner(i);
        temporaryRgb[i] = {
            unit[0] * peak / kReferenceLuminance,
            unit[1] * peak / kReferenceLuminance,
            unit[2] * peak / kReferenceLuminance
        };
        temporaryJMh[i] = RgbToJMh(temporaryRgb[i], params);
        // This assignment to 1 rather than i is present in the exact pinned
        // CTL and is therefore retained rather than silently normalized.
        if (temporaryJMh[i][2] < temporaryJMh[minimum][2]) minimum = 1;
    }
    for (std::size_t i = 0; i < kCuspCornerCount; ++i)
    {
        rgbCorners[i + 1] =
            temporaryRgb[(i + minimum) % kCuspCornerCount];
        jmhCorners[i + 1] =
            temporaryJMh[(i + minimum) % kCuspCornerCount];
    }
    rgbCorners[0] = rgbCorners[kCuspCornerCount];
    rgbCorners[kCuspCornerCount + 1] = rgbCorners[1];
    jmhCorners[0] = jmhCorners[kCuspCornerCount];
    jmhCorners[kCuspCornerCount + 1] = jmhCorners[1];
    jmhCorners[0][2] -= kHueLimit;
    jmhCorners[kCuspCornerCount + 1][2] += kHueLimit;
}

[[nodiscard]] std::array<Vec3, kTotalCornerCount> FindReachCorners(
    const JMhParams& params,
    const OdtParams& odt) noexcept
{
    std::array<Vec3, kCuspCornerCount> temporary{};
    std::array<Vec3, kTotalCornerCount> corners{};
    const double limitA =
        JToAchromatic(odt.limitJMax, params);
    std::size_t minimum = 0;
    for (std::size_t i = 0; i < kCuspCornerCount; ++i)
    {
        const Vec3 unit = GenerateCubeCorner(i);
        double lower = 0.0;
        double upper = odt.toneScale.forwardLimit;
        while (upper - lower > kReachCuspTolerance)
        {
            const double test = (lower + upper) / 2.0;
            const Vec3 sample{
                test * unit[0], test * unit[1], test * unit[2]
            };
            if (RgbToAab(sample, params)[0] < limitA) lower = test;
            else upper = test;
        }
        temporary[i] = RgbToJMh(
            {upper * unit[0], upper * unit[1], upper * unit[2]},
            params);
        if (temporary[i][2] < temporary[minimum][2]) minimum = i;
    }
    for (std::size_t i = 0; i < kCuspCornerCount; ++i)
        corners[i + 1] = temporary[(i + minimum) % kCuspCornerCount];
    corners[0] = corners[kCuspCornerCount];
    corners[kCuspCornerCount + 1] = corners[1];
    corners[0][2] -= kHueLimit;
    corners[kCuspCornerCount + 1][2] += kHueLimit;
    return corners;
}

[[nodiscard]] std::array<double, kMaxSortedCorners> ExtractSortedHues(
    const std::array<Vec3, kTotalCornerCount>& reach,
    const std::array<Vec3, kTotalCornerCount>& limit) noexcept
{
    std::array<double, kMaxSortedCorners> sorted{};
    std::size_t output = 0;
    std::size_t reachIndex = 1;
    std::size_t limitIndex = 1;
    while (reachIndex < kCuspCornerCount + 1 ||
           limitIndex < kCuspCornerCount + 1)
    {
        const double reachHue = reachIndex < kCuspCornerCount + 1
            ? reach[reachIndex][2]
            : std::numeric_limits<double>::infinity();
        const double limitHue = limitIndex < kCuspCornerCount + 1
            ? limit[limitIndex][2]
            : std::numeric_limits<double>::infinity();
        if (reachHue == limitHue)
        {
            sorted[output++] = reachHue;
            ++reachIndex;
            ++limitIndex;
        }
        else if (reachHue < limitHue)
        {
            sorted[output++] = reachHue;
            ++reachIndex;
        }
        else
        {
            sorted[output++] = limitHue;
            ++limitIndex;
        }
    }
    return sorted;
}

void BuildHueSampleInterval(
    std::array<double, kTotalTableSize>& table,
    int samples,
    Vec2 interval,
    int base) noexcept
{
    if (samples <= 0) return;
    const double delta = (interval[1] - interval[0]) / static_cast<double>(samples);
    for (int i = 0; i < samples; ++i)
        table[static_cast<std::size_t>(base + i)] =
            interval[0] + static_cast<double>(i) * delta;
}

[[nodiscard]] std::array<double, kTotalTableSize> BuildHueTable(
    const std::array<double, kMaxSortedCorners>& sorted) noexcept
{
    std::array<double, kTotalTableSize> table{};
    std::array<int, 2 * kCuspCornerCount + 2> sampleCounts{};
    const double idealSpacing =
        static_cast<double>(kTableSize) / kHueLimit;
    int minimumIndex = sorted[0] == 0.0 ? 0 : 1;
    int lastIndex = 0;
    for (std::size_t hueIndex = 0;
         hueIndex < kMaxSortedCorners;
         ++hueIndex)
    {
        int nominal = std::clamp(
            static_cast<int>(std::round(sorted[hueIndex] * idealSpacing)),
            minimumIndex,
            static_cast<int>(kTableSize - 1));
        if (lastIndex == nominal)
        {
            if (hueIndex > 1 &&
                sampleCounts[hueIndex - 2] !=
                    sampleCounts[hueIndex - 1] - 1)
                --sampleCounts[hueIndex - 1];
            else
                ++nominal;
        }
        sampleCounts[hueIndex] = std::min(
            nominal, static_cast<int>(kTableSize - 1));
        minimumIndex = nominal;
        lastIndex = minimumIndex;
    }

    int totalSamples = 0;
    std::size_t index = 0;
    BuildHueSampleInterval(
        table, sampleCounts[index], {0.0, sorted[index]}, totalSamples + 1);
    totalSamples += sampleCounts[index];
    for (++index; index < kMaxSortedCorners; ++index)
    {
        const int samples = sampleCounts[index] - sampleCounts[index - 1];
        BuildHueSampleInterval(
            table,
            samples,
            {sorted[index - 1], sorted[index]},
            totalSamples + 1);
        totalSamples += samples;
    }
    BuildHueSampleInterval(
        table,
        static_cast<int>(kTableSize) - totalSamples,
        {sorted[index - 1], kHueLimit},
        totalSamples + 1);
    table[0] = table[kBaseIndex + kTableSize - 1] - kHueLimit;
    table[kBaseIndex + kTableSize] = table[kBaseIndex] + kHueLimit;
    return table;
}

[[nodiscard]] Vec2 FindDisplayCusp(
    double hue,
    const std::array<Vec3, kTotalCornerCount>& rgbCorners,
    const std::array<Vec3, kTotalCornerCount>& jmhCorners,
    const JMhParams& params) noexcept
{
    const auto upper = std::find_if(jmhCorners.begin() + 1, jmhCorners.end(),
        [hue](const Vec3& corner) { return corner[2] > hue; });
    const std::size_t upperCorner = upper == jmhCorners.end() ? 1 :
        static_cast<std::size_t>(upper - jmhCorners.begin());
    const std::size_t lowerCorner = upperCorner - 1;
    if (jmhCorners[lowerCorner][2] == hue)
        return {jmhCorners[lowerCorner][0], jmhCorners[lowerCorner][1]};

    double lowerT = 0.0;
    double upperT = 1.0;
    while (upperT - lowerT > kDisplayCuspTolerance)
    {
        const double sampleT = (lowerT + upperT) / 2.0;
        const Vec3 sample{
            std::lerp(rgbCorners[lowerCorner][0],
                      rgbCorners[upperCorner][0], sampleT),
            std::lerp(rgbCorners[lowerCorner][1],
                      rgbCorners[upperCorner][1], sampleT),
            std::lerp(rgbCorners[lowerCorner][2],
                      rgbCorners[upperCorner][2], sampleT)
        };
        const Vec3 jmh = RgbToJMh(sample, params);
        if (jmh[2] < jmhCorners[lowerCorner][2])
            upperT = sampleT;
        else if (jmh[2] >= jmhCorners[upperCorner][2])
            lowerT = sampleT;
        else if (jmh[2] > hue)
            upperT = sampleT;
        else
            lowerT = sampleT;
    }
    const double sampleT = (lowerT + upperT) / 2.0;
    const Vec3 sample{
        std::lerp(rgbCorners[lowerCorner][0],
                  rgbCorners[upperCorner][0], sampleT),
        std::lerp(rgbCorners[lowerCorner][1],
                  rgbCorners[upperCorner][1], sampleT),
        std::lerp(rgbCorners[lowerCorner][2],
                  rgbCorners[upperCorner][2], sampleT)
    };
    const Vec3 jmh = RgbToJMh(sample, params);
    return {jmh[0], jmh[1]};
}

[[nodiscard]] std::array<Vec3, kTotalTableSize> BuildCuspTable(
    const std::array<double, kTotalTableSize>& hues,
    const std::array<Vec3, kTotalCornerCount>& rgbCorners,
    const std::array<Vec3, kTotalCornerCount>& jmhCorners,
    const JMhParams& params) noexcept
{
    std::array<Vec3, kTotalTableSize> table{};
    for (std::size_t i = kBaseIndex; i < kTotalTableSize; ++i)
    {
        const Vec2 cusp = FindDisplayCusp(
            hues[i], rgbCorners, jmhCorners, params);
        table[i] = {
            cusp[0],
            cusp[1] * (1.0 + kSmoothM * kSmoothCusps),
            hues[i]
        };
    }
    table[0] = {table[kTableSize][0], table[kTableSize][1], hues[0]};
    table[kBaseIndex + kTableSize] = {
        table[kBaseIndex][0], table[kBaseIndex][1],
        hues[kBaseIndex + kTableSize]
    };
    return table;
}

[[nodiscard]] std::array<Vec3, kTotalTableSize> BuildGamutCuspTable(
    const JMhParams& reach,
    const JMhParams& limit,
    const OdtParams& params,
    std::array<double, kTotalTableSize>& hues) noexcept
{
    const auto reachCorners = FindReachCorners(reach, params);
    std::array<Vec3, kTotalCornerCount> limitRgb{};
    std::array<Vec3, kTotalCornerCount> limitJMh{};
    BuildLimitingCorners(
        limitRgb, limitJMh, limit, params.peakLuminance);
    const auto sorted = ExtractSortedHues(reachCorners, limitJMh);
    hues = BuildHueTable(sorted);
    return BuildCuspTable(hues, limitRgb, limitJMh, limit);
}

[[nodiscard]] bool HasNegativeChannel(Vec3 rgb) noexcept
{
    return rgb[0] < 0.0 || rgb[1] < 0.0 || rgb[2] < 0.0;
}

[[nodiscard]] Vec2 BracketReachChroma(
    double hue,
    const JMhParams& params,
    double limitJ) noexcept
{
    double low = 0.0;
    double high = 50.0;
    bool outside = false;
    while (!outside && high < 1300.0)
    {
        const Vec3 rgb = JMhToRgb({limitJ, high, hue}, params);
        outside = HasNegativeChannel(rgb);
        if (!outside)
        {
            low = high;
            high += 50.0;
        }
    }
    return {low, high};
}

[[nodiscard]] double FindReachChroma(
    double hue,
    const JMhParams& params,
    double limitJ) noexcept
{
    auto [low, high] = BracketReachChroma(hue, params, limitJ);
    while (high - low > 1.0e-2)
    {
        const double sample = (high + low) / 2.0;
        const Vec3 rgb = JMhToRgb({limitJ, sample, hue}, params);
        if (HasNegativeChannel(rgb))
            high = sample;
        else
            low = sample;
    }
    return high;
}

[[nodiscard]] std::array<double, kTotalTableSize> BuildReachMTable(
    const JMhParams& params,
    double limitJ) noexcept
{
    std::array<double, kTotalTableSize> table{};
    for (std::size_t i = 0; i < kTableSize; ++i)
        table[i + kBaseIndex] = FindReachChroma(BaseHue(i), params, limitJ);
    table[0] = table[kTableSize];
    table[kBaseIndex + kTableSize] = table[kBaseIndex];
    return table;
}

struct GammaTestData
{
    Vec3 jmh{};
    double sourceIntersectJ{};
    double slope{};
    double cuspIntersectJ{};
};

[[nodiscard]] std::array<GammaTestData, 5> BuildGammaTestData(
    Vec2 cusp,
    double hue,
    const OdtParams& params) noexcept
{
    constexpr std::array<double, 5> kPositions{
        0.01, 0.1, 0.5, 0.8, 0.99
    };
    std::array<GammaTestData, 5> data{};
    const double analyticalThreshold =
        std::lerp(cusp[0], params.limitJMax, kFocusGainBlend);
    const double focusJ = std::lerp(
        cusp[0], params.midJ,
        std::min(1.0, kCuspMidBlend - cusp[0] / params.limitJMax));
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const double testJ = std::lerp(cusp[0], params.limitJMax, kPositions[i]);
        const double gain = FocusGain(
            testJ, analyticalThreshold, params);
        const CompressionAxis axis{focusJ, params.limitJMax, gain};
        const double intersect = SolveJIntersect(
            {testJ, cusp[1]}, axis);
        data[i] = {
            .jmh = {testJ, cusp[1], hue},
            .sourceIntersectJ = intersect,
            .slope = CompressionVectorSlope(
                intersect, axis),
            .cuspIntersectJ = SolveJIntersect(
                cusp, axis)
        };
    }
    return data;
}

[[nodiscard]] bool EvaluateGammaFit(
    Vec2 cusp,
    const std::array<GammaTestData, 5>& data,
    double upperGammaInverse,
    const OdtParams& params) noexcept
{
    const double luminanceLimit =
        params.peakLuminance / kReferenceLuminance;
    for (const auto& test : data)
    {
        const double approximateM = FindBoundaryIntersectionM(
            {cusp, params.limitJMax, upperGammaInverse, params.lowerHullGammaInverse},
            {test.sourceIntersectJ, test.slope},
            test.cuspIntersectJ);
        const double approximateJ =
            test.sourceIntersectJ + test.slope * approximateM;
        const Vec3 rgb = JMhToRgb(
            {approximateJ, approximateM, test.jmh[2]}, params.limit);
        if (!std::any_of(rgb.begin(), rgb.end(),
                [luminanceLimit](double channel) { return channel > luminanceLimit; }))
            return false;
    }
    return true;
}

[[nodiscard]] Vec2 BracketUpperHullGamma(
    Vec2 cusp,
    const std::array<GammaTestData, 5>& testData,
    const OdtParams& params) noexcept
{
    double low = kGammaMinimum;
    double high = low + kGammaSearchStep;
    bool found = false;
    while (!found && high < kGammaMaximum)
    {
        found = EvaluateGammaFit(
            cusp, testData, 1.0 / high, params);
        if (!found)
        {
            low = high;
            high += kGammaSearchStep;
        }
    }
    return {low, high};
}

[[nodiscard]] double FindUpperHullGammaInverse(
    Vec2 cusp,
    const std::array<GammaTestData, 5>& testData,
    const OdtParams& params) noexcept
{
    auto [low, high] = BracketUpperHullGamma(cusp, testData, params);
    while (high - low > kGammaAccuracy)
    {
        const double test = (high + low) / 2.0;
        if (EvaluateGammaFit(cusp, testData, 1.0 / test, params))
            high = test;
        else
            low = test;
    }
    return 1.0 / high;
}

[[nodiscard]] std::array<double, kTotalTableSize> BuildUpperHullGamma(
    const OdtParams& params) noexcept
{
    std::array<double, kTotalTableSize> table{};
    for (std::size_t i = kBaseIndex;
         i < kBaseIndex + kTableSize;
         ++i)
    {
        const double hue = params.gamutCusps[i][2];
        const Vec2 cusp{
            params.gamutCusps[i][0], params.gamutCusps[i][1]
        };
        const auto testData = BuildGammaTestData(
            cusp, hue, params);
        table[i] = FindUpperHullGammaInverse(cusp, testData, params);
    }
    table[0] = table[kTableSize];
    table[kTableSize + kBaseIndex] = table[kBaseIndex];
    return table;
}

[[nodiscard]] OdtParams InitializeOdt(double peak) noexcept
{
    OdtParams params{};
    params.peakLuminance = peak;
    params.input = InitializeJMh(kAp0);
    params.reach = InitializeJMh(kAp1);
    params.limit = InitializeJMh(kBt2020);
    params.toneScale = InitializeToneScale(peak);
    params.limitJMax = YToJ(peak, params.input);
    params.modelGammaInverse = 1.0 / kModelGamma;
    params.reachM = BuildReachMTable(params.reach, params.limitJMax);
    params.saturation = std::max(
        0.2,
        kChromaExpand -
            kChromaExpand * kChromaExpandFactor *
                params.toneScale.logPeak);
    params.saturationThreshold = kChromaExpandThreshold / peak;
    params.compression =
        kChromaCompress +
        kChromaCompress * kChromaCompressFactor *
            params.toneScale.logPeak;
    params.chromaCompressionScale =
        std::pow(0.03379 * peak, 0.30596) - 0.45135;
    params.midJ = YToJ(
        params.toneScale.ct * kReferenceLuminance, params.input);
    params.focusDistance =
        kFocusDistance +
        kFocusDistance * kFocusDistanceScaling *
            params.toneScale.logPeak;
    params.lowerHullGammaInverse = 1.0 /
        (1.14 + 0.07 * params.toneScale.logPeak);
    params.gamutCusps = BuildGamutCuspTable(
        params.reach, params.limit, params, params.hues);
    params.upperHullGamma = BuildUpperHullGamma(params);
    return params;
}

}  // namespace fo4hdr::aces2
