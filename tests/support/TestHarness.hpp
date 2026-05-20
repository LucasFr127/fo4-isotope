// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string_view>

namespace fo4hdr::test {

inline int failures = 0;

inline void Check(
    bool condition,
    std::string_view expression,
    std::string_view file,
    int line)
{
    if (!condition) {
        ++failures;
        std::cerr << file << ':' << line
                  << ": FAIL: " << expression << '\n';
    }
}

inline bool Approx(double a, double b, double epsilon = 1.0e-10)
{
    return std::isfinite(a) && std::isfinite(b) &&
           std::isfinite(epsilon) && epsilon >= 0.0 &&
           std::abs(a - b) <=
           epsilon * std::max({1.0, std::abs(a), std::abs(b)});
}

}  // namespace fo4hdr::test

#define FO4HDR_CHECK(expression) \
    ::fo4hdr::test::Check( \
        static_cast<bool>(expression), \
        #expression, \
        __FILE__, \
        __LINE__)
