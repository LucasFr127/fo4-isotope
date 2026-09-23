// SPDX-License-Identifier: GPL-3.0-or-later
#include "Fallout4HDR/runtime/RuntimeValidation.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <string>

namespace fo4hdr::tests {
void RunRuntimeValidationTests()
{
    // Authored comparison data is unrelated to any executable identity.
    const std::string hash(64, 'a');
    const RuntimeIdentity expected{"1.10.163", hash, "0.6.23", hash, hash,
        "version-1-10-163-0.bin", hash, hash};
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, expected).valid);
    FO4HDR_CHECK(ValidateRuntimeIdentity({}, expected).reason == RuntimeValidationReason::ProfileUninitialized);
    auto observed = expected;
    observed.runtimeVersion = "unknown";
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, observed).reason == RuntimeValidationReason::RuntimeVersionMismatch);
    observed = expected;
    observed.executableSha256 = {};
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, observed).reason == RuntimeValidationReason::ExecutableUnavailable);
    FO4HDR_CHECK(ValidateRuntimeAction("runtime-pinned-unvalidated", RuntimeAction::MilestoneValidation).allowed);
    FO4HDR_CHECK(!ValidateRuntimeAction("runtime-pinned-unvalidated", RuntimeAction::ReleasePackaging).allowed);
    FO4HDR_CHECK(ValidateRuntimeAction("release-approved", RuntimeAction::ReleasePackaging).allowed);
    FO4HDR_CHECK(!ValidateRuntimeAction("planned", RuntimeAction::MilestoneValidation).allowed);
}
} // namespace fo4hdr::tests
