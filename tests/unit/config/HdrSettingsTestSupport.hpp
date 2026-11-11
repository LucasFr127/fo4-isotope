// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <string>

namespace fo4hdr::tests {

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
