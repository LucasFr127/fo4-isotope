// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "NativeGradeReference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace fo4hdr::tests::gpu {

[[nodiscard]] NativeGradeRgb SyntheticNativeGrade(NativeGradeRgb encoded, unsigned grade)
{
    // Affine functions isolate subtexel sampler quantization from LUT curvature.
    // Separate channel slopes and offsets detect slot/weight/channel mistakes.
    constexpr std::array<NativeGradeRgb, 4> slopes{{
        {1, 1, 1}, {0.9, 0.7, 0.5}, {0.5, 0.75, 0.95}, {0.65, 0.65, 0.65}}};
    constexpr std::array<NativeGradeRgb, 4> offsets{{
        {0, 0, 0}, {0.04, 0.01, 0}, {0, 0.02, 0.04}, {0.08, 0.08, 0.08}}};
    for (std::size_t c = 0; c < 3; ++c)
        encoded[c] = encoded[c] * slopes[grade][c] + offsets[grade][c];
    return encoded;
}

[[nodiscard]] double NativeGradeClean(double value)
{
    return std::isnan(value) ? 0.0 : std::clamp(value, 0.0, 65504.0);
}

[[nodiscard]] double NativeGradeScale(double peak)
{
    if (peak >= 5.6) return 1.0 / peak;
    constexpr double white = 11.2 * (0.14 * 11.2 + 1.0 / 60.0) /
        (11.2 * (0.15 * 11.2 + 0.5) + 0.06);
    const double x = 2.0 * peak;
    return 2.0 * (0.14 * x + 1.0 / 60.0) /
        ((x * (0.15 * x + 0.5) + 0.06) * white);
}

[[nodiscard]] NativeGradeRgb DecodeAndTransferNativeGrade(
    NativeGradeRgb source, NativeGradeRgb graded, double strength)
{
    for (double& channel : source) channel = NativeGradeClean(channel);
    strength = std::isnan(strength) ? 0.0 : std::clamp(strength, 0.0, 1.0);
    if (strength == 0) return source;
    const double scale = NativeGradeScale(*std::max_element(source.begin(), source.end()));
    for (double& channel : graded) channel = std::pow(NativeGradeClean(channel), 2.2) / scale;
    const double bound = std::min(1.0, 65504.0 /
        std::max(65504.0, *std::max_element(graded.begin(), graded.end())));
    for (std::size_t c = 0; c < 3; ++c)
        graded[c] = source[c] + strength * (std::min(65504.0, graded[c] * bound) - source[c]);
    return graded;
}

[[nodiscard]] NativeGradeRgb SampleSyntheticNativeGrade(
    const NativeGradeRgb& source, const std::array<float, 4>& weights)
{
    const double scale = NativeGradeScale(*std::max_element(source.begin(), source.end()));
    NativeGradeRgb encoded{}, graded{};
    for (std::size_t c = 0; c < 3; ++c) {
        encoded[c] = std::pow(std::clamp(source[c] * scale, 0.0, 1.0), 1.0 / 2.2);
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        const auto sample = SyntheticNativeGrade(encoded, slot);
        for (std::size_t c = 0; c < 3; ++c) graded[c] += sample[c] * weights[slot];
    }
    return graded;
}

[[nodiscard]] NativeGradeRgb FrozenNativeGrade(
    NativeGradeRgb source, const std::array<float, 4>& weights, double strength)
{
    for (double& channel : source) channel = NativeGradeClean(channel);
    const auto graded = SampleSyntheticNativeGrade(source, weights);
    return DecodeAndTransferNativeGrade(source,graded,strength);
}

}  // namespace fo4hdr::tests::gpu
