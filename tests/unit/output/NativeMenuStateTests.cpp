// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "FO4Isotope/output/NativeMenuState.hpp"
#include "TestHarness.hpp"

namespace fo4hdr::tests {

void RunNativeMenuStateTests()
{
    ResetNativeMenuState();
    const auto staleGeneration = NativeMenuGeneration();
    FO4HDR_CHECK(ReadNativeMenuState() == NativeMenuState::unknown);
    ResetNativeMenuState();
    // A publication from the retired generation is rejected.
    PublishNativeMenuState(staleGeneration, false);
    FO4HDR_CHECK(ReadNativeMenuState() == NativeMenuState::unknown);
    const auto generation = NativeMenuGeneration();
    FO4HDR_CHECK(generation != staleGeneration && generation != 0U);
    PublishNativeMenuState(generation, true);
    FO4HDR_CHECK(ReadNativeMenuState() == NativeMenuState::active);
    FO4HDR_CHECK(NativeMenuGeneration() == generation);
    PublishNativeMenuState(generation, false);
    FO4HDR_CHECK(ReadNativeMenuState() == NativeMenuState::inactive);
    ResetNativeMenuState();
    FO4HDR_CHECK(ReadNativeMenuState() == NativeMenuState::unknown);
}

}  // namespace fo4hdr::tests
