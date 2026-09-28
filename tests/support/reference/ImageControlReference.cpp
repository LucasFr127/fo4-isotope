// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ImageControlReference.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fo4hdr::tests::gpu {

double ImageControlLuminance(const NativeGradeRgb &v)
{
    return v[0] * 0.212500006 + v[1] * 0.715399981 + v[2] * 0.0720999986;
}

bool NeutralShadowContrast(double shadows, double contrast)
{
    return shadows == 0 && contrast == 1;
}

void ApplyImageShadowContrast(NativeGradeRgb &color, double shadows, double contrast)
{
    const double t = std::clamp((ImageControlLuminance(color) - 0.02) / 0.16, 0.0, 1.0);
    const double shadowGain = std::exp2(shadows * (1.0 - t * t * (3.0 - 2.0 * t)));
    for (auto &c : color)
        c *= shadowGain;
    const double y = ImageControlLuminance(color);
    if (y > 0) {
        const double target = 0.18 * std::pow(std::max(y / 0.18, 1.0e-6), contrast);
        for (auto &c : color)
            c *= target / y;
    }
}

[[nodiscard]] NativeGradeRgb ApplyImageControlsOracle(NativeGradeRgb color, double shadows, double contrast,
                                                      double blacks)
{
    if (NeutralShadowContrast(shadows, contrast) && blacks == 0)
        return color;
    if (!NeutralShadowContrast(shadows, contrast))
        ApplyImageShadowContrast(color, shadows, contrast);
    const double controlledY = ImageControlLuminance(color);
    const double toe = std::clamp(1.0 - controlledY / 0.02, 0.0, 1.0);
    for (auto &c : color)
        c *= 1.0 + blacks * toe * toe;
    return color;
}

NativeGradeRgb NormalizeRemovalColor(const NativeGradeRgb &color, double luminance, const NativeGradeRgb &fallback)
{
    if (!(luminance > 0))
        return fallback;
    NativeGradeRgb result{};
    for (std::size_t channel = 0; channel < 3; ++channel)
        result[channel] = color[channel] / luminance;
    return result;
}

void ContainRemovalColor(NativeGradeRgb &result)
{
    const double maximum = *std::max_element(result.begin(), result.end());
    const double scale = std::min(1.0, 65504.0 / std::max(maximum, 65504.0));
    for (auto &channel : result)
        channel = std::min(channel * scale, 65504.0);
}

[[nodiscard]] NativeGradeRgb ApplyVanillaGradeRemovalOracle(const NativeGradeRgb &hdr, const NativeGradeRgb &graded,
                                                            double luminanceRemoval, double colorRemoval)
{
    if (luminanceRemoval <= 0 && colorRemoval <= 0)
        return graded;
    if (luminanceRemoval >= 1 && colorRemoval >= 1)
        return hdr;
    constexpr NativeGradeRgb weights{0.212500006, 0.715399981, 0.0720999986};
    const auto luminance = [&](const NativeGradeRgb &value) {
        return value[0] * weights[0] + value[1] * weights[1] + value[2] * weights[2];
    };
    const double yh = luminance(hdr);
    const double ye = luminance(graded);
    if (yh <= 0 && ye <= 0)
        return {};

    const auto ch = NormalizeRemovalColor(hdr, yh, {1, 1, 1});
    const auto ce = NormalizeRemovalColor(graded, ye, ch);

    const double yt = std::lerp(yh, ye, 1 - std::clamp(luminanceRemoval, 0.0, 1.0));
    NativeGradeRgb result{};
    for (std::size_t channel = 0; channel < 3; ++channel)
        result[channel] = yt * std::lerp(ch[channel], ce[channel], 1 - std::clamp(colorRemoval, 0.0, 1.0));
    ContainRemovalColor(result);
    return result;
}

}  // namespace fo4hdr::tests::gpu
