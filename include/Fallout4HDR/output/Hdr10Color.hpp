// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fo4hdr {

inline constexpr double kSceneReferenceWhiteNits = 203.0;
inline constexpr double kMinimumDisplayPeakNits = 400.0;
inline constexpr double kMaximumDisplayPeakNits = 2000.0;
inline constexpr double kMinimumUiPaperWhiteNits = 100.0;
inline constexpr double kMaximumUiPaperWhiteNits = 300.0;

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


// Contains absolute linear BT.2020 channels at the physical display boundary.
// Display peaks outside (0, 10000] fail to black.
[[nodiscard]] LinearRgb ContainBt2020Nits(
    LinearRgb value, double displayPeakNits) noexcept;

// Encodes absolute luminance in nits as the ST 2084 signal in [0, 1].
// Negative and NaN inputs use black; values above 10000 nits use the ceiling.
[[nodiscard]] double EncodeSt2084Pq(double nits) noexcept;
[[nodiscard]] LinearRgb EncodeSt2084Pq(LinearRgb nits) noexcept;

[[nodiscard]] std::uint16_t QuantizeR10Unorm(double encoded) noexcept;
[[nodiscard]] double NormalizeR10Unorm(std::uint16_t code) noexcept;

}  // namespace fo4hdr
