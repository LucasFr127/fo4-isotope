// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/PluginConfiguration.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fo4hdr::tests {

// One rendered section, from its header through the line before the next one.
[[nodiscard]] inline std::string IniSection(const std::string& text, std::string_view header)
{
    const auto begin = text.find("[" + std::string(header) + "]\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find("\r\n[", begin);
    return text.substr(begin, end == std::string::npos ? end : end + 2 - begin);
}

// True when the rendered text holds exactly this line.
[[nodiscard]] inline bool HasIniLine(std::string_view text, std::string_view line)
{
    for (std::size_t start = 0; start < text.size();)
    {
        const auto end = text.find("\r\n", start);
        if (text.substr(start, end - start) == line) return true;
        if (end == std::string_view::npos) break;
        start = end + 2;
    }
    return false;
}

[[nodiscard]] inline std::uint32_t RepairCount(
    const PluginConfigurationResult& result, ConfigurationRepairKind kind)
{
    return result.repairs.counts[static_cast<std::size_t>(kind)];
}

[[nodiscard]] inline std::string MinimalProductionConfiguration()
{
    return R"ini([General]
HdrEnabled=1
DeveloperMode=0
[Output]
DisplayPeakNits=1000
UiPaperWhiteNits=203
[ToneMapper]
Algorithm=ACES2
[Image]
ExposureEV=0
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ForceHdrSwapChainFailure=0
)ini";
}

[[nodiscard]] inline std::string ExtendedProductionConfiguration()
{
    auto text = MinimalProductionConfiguration();
    text += R"ini([Output]
VideoWhiteNits=250
[Image]
Contrast=-25
Blacks=-40
Highlights=50
Shadows=-75
Saturation=125
HighlightSaturation=40
Temperature=20
Tint=-30
ColorGradingMethod=Affine
ColorGradingCurve=sRGB
GradeTransfer=PreserveHDRColor
[Effects]
VanillaColorGradingStrength=60
FallbackGradeLuminance=75
FallbackGradeColor=40
BloomStrength=75
[ACES2]
ContrastLookStrength=0.25
[AgX]
MiddleGrayOffsetEV=-0.25
Contrast=2.9
ColorResponse=0.6
ShoulderPower=2.1
[GT7]
ToeStrength=1.4
ShoulderStart=0.55
ColorBlend=0.25
)ini";
    return text;
}

}  // namespace fo4hdr::tests
