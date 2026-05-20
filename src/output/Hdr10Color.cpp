// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <algorithm>
#include <cmath>

namespace fo4hdr {
namespace {

double BoundLight(double nits, double ceiling) noexcept
{
    return nits > 0.0 ? std::min(nits, ceiling) : 0.0;
}

}  // namespace

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

}  // namespace fo4hdr
