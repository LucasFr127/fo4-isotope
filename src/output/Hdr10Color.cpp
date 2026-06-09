// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fo4hdr {
namespace {

using Matrix = std::array<std::array<double, 3>, 3>;

// Derived from the specified primaries and D65 by solving the XYZ white scale.
// Binary32 rounding fixes the coefficient precision of the transform.
// https://www.itu.int/rec/R-REC-BT.709-6-201506-I/en
// https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en
constexpr Matrix primaryTransform{{
    {0.627403895934699F, 0.329283038377884F, 0.043313065687417F},
    {0.069097289358232F, 0.919540395075459F, 0.011362315566309F},
    {0.016391438875150F, 0.088013307877226F, 0.895595253247624F}
}};

// Invert the rounded forward transform for comparison round trips.
constexpr Matrix comparisonTransform{{
    {1.6604909457791666, -0.5876410948776531, -0.0728498646690886},
    {-0.12455046636659303, 1.1328998802803918, -0.008349422033247976},
    {-0.01815076427030725, -0.10057889487266629, 1.118729662272663}
}};

LinearRgb Transform(const Matrix& matrix, LinearRgb rgb) noexcept
{
    const auto channel = [&](const auto& row) {
        return row[0] * rgb.red + row[1] * rgb.green + row[2] * rgb.blue;
    };
    return {channel(matrix[0]), channel(matrix[1]), channel(matrix[2])};
}

double BoundLight(double nits, double ceiling) noexcept
{
    return nits > 0.0 ? std::min(nits, ceiling) : 0.0;
}

}  // namespace

LinearRgb ConvertBt709ToBt2020(LinearRgb value) noexcept
{
    return Transform(primaryTransform, value);
}

LinearRgb ConvertBt2020ToBt709ForComparison(LinearRgb value) noexcept
{
    return Transform(comparisonTransform, value);
}

LinearRgb ContainBt2020Nits(LinearRgb value, double displayPeakNits) noexcept
{
    if (!(displayPeakNits > 0.0 && displayPeakNits <= 10000.0))
    {
        return {};
    }
    return {BoundLight(value.red, displayPeakNits),
        BoundLight(value.green, displayPeakNits), BoundLight(value.blue, displayPeakNits)};
}

double EncodeSt2084Pq(double nits) noexcept
{
    // Inverse display EOTF, BT.2100 Table 4, with absolute luminance units.
    // https://www.itu.int/rec/R-REC-BT.2100-3-202502-I/en
    const double light = BoundLight(nits, 10000.0) / 10000.0;
    const double power = std::pow(light, 2610.0 / 16384.0);
    const double ratio = (3424.0 / 4096.0 + power * (2413.0 / 128.0)) /
        (1.0 + power * (2392.0 / 128.0));
    return std::clamp(std::pow(ratio, 2523.0 / 32.0), 0.0, 1.0);
}

LinearRgb EncodeSt2084Pq(LinearRgb nits) noexcept
{
    return {EncodeSt2084Pq(nits.red), EncodeSt2084Pq(nits.green),
        EncodeSt2084Pq(nits.blue)};
}

std::uint16_t QuantizeR10Unorm(double encoded) noexcept
{
    // D3D FLOAT-to-UNORM uses nearest integer after range containment.
    // https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-data-conversion
    return static_cast<std::uint16_t>(
        std::floor(BoundLight(encoded, 1.0) * 1023.0 + 0.5));
}

double NormalizeR10Unorm(std::uint16_t code) noexcept
{
    return std::min<double>(code, 1023.0) / 1023.0;
}

}  // namespace fo4hdr
