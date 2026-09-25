// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <string>

namespace fo4hdr::tests {

[[nodiscard]] inline std::string LegacyProductionConfiguration()
{
    return R"ini([General]
Enabled=1
DeveloperMode=0
[Output]
DisplayPeakNits=1000
UiPaperWhiteNits=203
[ToneMapper]
Algorithm=ACES2LMT
[Image]
Exposure=0
[Debug]
TraceRenderPipeline=0
DeveloperHdr10Reference=0
ShowDeveloperOverlay=0
ForceHdrSwapChainFailure=0
)ini";
}

[[nodiscard]] inline std::string ExtendedProductionConfiguration()
{
    auto text = LegacyProductionConfiguration();
    text += R"ini([Output]
VideoWhiteNits=250
[Image]
Contrast=-25
Blacks=-40
Highlights=50
Shadows=-75
Saturation=125
Temperature=20
Tint=-30
ColorGradingMethod=NativeReference
GradeTransfer=PreserveHDRColor
[Effects]
VanillaColorGradingStrength=60
VanillaGradeLuminance=75
VanillaGradeColor=40
BloomStrength=175
[ACES2LMT]
LookStrength=0.25
[AgX]
MiddleGray=-0.25
Contrast=2.9
HdrPurity=0.6
ShoulderPower=2.1
[GT7]
ToeStrength=1.4
ShoulderStart=0.55
ColorBlend=0.25
)ini";
    return text;
}

}  // namespace fo4hdr::tests
