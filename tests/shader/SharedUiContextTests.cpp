// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"

namespace fo4hdr::tests::gpu {
void VerifySharedUiContext(Device& device)
{
    SharedPresentationContextFixture fixture(device);
    fixture.PrepareScene();
    fixture.DrawHud();
    fixture.PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4));
    fixture.CheckFrozenWhitePresentation();
    fixture.PrepareScene();
    fixture.DrawHud(false);
    FO4HDR_WARP_CHECK(SharedUiFrameReady());
    fixture.PresentEncodedUi(0.5);
    fixture.PrepareScene();
    fixture.DrawHud(true, ArmorUiOwner::other);
    FO4HDR_WARP_CHECK(!SharedUiFrameEncoded());
    fixture.PresentReadable(0.5);
    CheckDebugMessages(device);
}
} // namespace fo4hdr::tests::gpu
