// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fo4hdr {

struct LinearRgb
{
    double red{};
    double green{};
    double blue{};
};

// D65 linear-light primary transforms. The CPU inverse is a comparison and
// validation reference.
[[nodiscard]] LinearRgb ConvertBt709ToBt2020(
    LinearRgb value) noexcept;
[[nodiscard]] LinearRgb ConvertBt2020ToBt709ForComparison(
    LinearRgb value) noexcept;


// Encodes absolute luminance in nits as the ST 2084 signal in [0, 1].
// Negative and NaN inputs use black; values above 10000 nits use the ceiling.
[[nodiscard]] double EncodeSt2084Pq(double nits) noexcept;

}  // namespace fo4hdr
