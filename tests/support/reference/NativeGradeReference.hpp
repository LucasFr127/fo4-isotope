// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace fo4hdr::tests::gpu {

using NativeGradeRgb = std::array<double, 3>;

[[nodiscard]] NativeGradeRgb SyntheticNativeGrade(NativeGradeRgb encoded, unsigned grade);

[[nodiscard]] double NativeGradeClean(double value);

[[nodiscard]] double NativeGradeScale(double peak);

[[nodiscard]] NativeGradeRgb DecodeAndTransferNativeGrade(
    NativeGradeRgb source, NativeGradeRgb graded, double strength);

[[nodiscard]] NativeGradeRgb SampleSyntheticNativeGrade(
    const NativeGradeRgb& source, const std::array<float, 4>& weights);

[[nodiscard]] NativeGradeRgb FrozenNativeGrade(
    NativeGradeRgb source, const std::array<float, 4>& weights, double strength);

}  // namespace fo4hdr::tests::gpu
