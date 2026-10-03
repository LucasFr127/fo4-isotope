// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
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

// Contains each absolute linear BT.2020 channel to [0, displayPeakNits] at the
// physical display boundary. Display peaks outside (0, 10000] fail to black.
[[nodiscard]] LinearRgb ContainBt2020Nits(
    LinearRgb value,
    double displayPeakNits) noexcept;

// SMPTE ST 2084 maps absolute luminance in [0, 10000] nits to [0, 1]. The
// final output boundary clamps only this encoded result, never the FP16 scene.
[[nodiscard]] double EncodeSt2084Pq(double nits) noexcept;
[[nodiscard]] LinearRgb EncodeSt2084Pq(LinearRgb nits) noexcept;

// BT.2124 potential visibility metric for absolute linear BT.2020 nits.
// Invalid channels outside finite [0, 10000] return positive infinity.
[[nodiscard]] double DeltaEItpBt2020Nits(
    LinearRgb reference, LinearRgb candidate) noexcept;

[[nodiscard]] std::uint16_t QuantizeR10Unorm(double encoded) noexcept;
[[nodiscard]] double NormalizeR10Unorm(std::uint16_t code) noexcept;

}  // namespace fo4hdr
