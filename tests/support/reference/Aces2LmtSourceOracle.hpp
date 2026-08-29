// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright Contributors to the ACES Project
//
// Upstream: scottdyer/aces-look@dc79dd74554e73a9e1518ec5f00ae0c1725f1686/contrast/LMT_Test_OCIO_options.dctl
// Source SHA-256:
// b5b1022c46758c02edd4c3967375d6daa0cd022e78c85962004a2f628318711f.
// Modified by Fallout4HDR: this test-only C++ port selects only
// J2M2_CURVE ("J curve 2 + M curve 2"), preserves the source float constants
// and evaluation order, and replaces DCTL vector/intrinsic syntax with C++.
#pragma once

#include "Fallout4HDR/output/Hdr10Color.hpp"

namespace fo4hdr::tests {

// Independent source-precision oracle for the pinned candidate LMT's selected
// J2M2_CURVE branch. Input and output are dimensionless scene-linear
// ACES2065-1 (AP0/D60). Deliberately performs source-style float evaluation
// and does not apply the Academy Output Transform, display adaptation,
// containment, gamut conversion, or PQ encoding.
[[nodiscard]] LinearRgb ApplyAces2ContrastLmtSourceFloat(
    LinearRgb sceneLinearAp0) noexcept;

} // namespace fo4hdr::tests
