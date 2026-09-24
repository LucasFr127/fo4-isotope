// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "NativeGradeReference.hpp"

namespace fo4hdr::tests::gpu {

double ImageControlLuminance(const NativeGradeRgb &v);
bool NeutralShadowContrast(double shadows, double contrast);

[[nodiscard]] NativeGradeRgb ApplyImageControlsOracle(NativeGradeRgb color, double shadows, double contrast,
                                                      double blacks);

[[nodiscard]] NativeGradeRgb ApplyVanillaGradeRemovalOracle(const NativeGradeRgb &hdr, const NativeGradeRgb &graded,
                                                            double luminanceRemoval, double colorRemoval);

}  // namespace fo4hdr::tests::gpu
