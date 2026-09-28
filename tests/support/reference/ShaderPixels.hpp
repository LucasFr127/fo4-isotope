// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <array>

namespace fo4hdr::tests::gpu {

inline constexpr unsigned kWidth = 8;
inline constexpr unsigned kHeight = 1;
inline constexpr float kReferenceWhiteNits = 203.0F;

struct Float4
{
    float red;
    float green;
    float blue;
    float alpha;
};

inline constexpr std::array<Float4, kWidth> kSamples{{
    {0.0F, 0.0F, 0.0F, 1.0F},
    {0.18F, 0.18F, 0.18F, 1.0F},
    {1.0F, 1.0F, 1.0F, 1.0F},
    {4.0F, 4.0F, 4.0F, 1.0F},
    {16.0F, 16.0F, 16.0F, 1.0F},
    {4.0F, 0.5F, 0.125F, 1.0F},
    {0.125F, 4.0F, 0.5F, 1.0F},
    {0.5F, 0.125F, 4.0F, 1.0F},
}};

}  // namespace fo4hdr::tests::gpu
