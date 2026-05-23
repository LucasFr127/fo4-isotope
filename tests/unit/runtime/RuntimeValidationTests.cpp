// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/runtime/RuntimeValidation.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <cstddef>

namespace {

constexpr fo4hdr::RuntimeIdentity supportedIdentity{{1, 10, 163, 0}, {0, 6, 23, 0}};

void TestSupportedIdentity()
{
    const auto result = fo4hdr::ValidateRuntimeIdentity(supportedIdentity);
    FO4HDR_CHECK(result.valid);
    FO4HDR_CHECK(result.reason == fo4hdr::RuntimeValidationReason::Valid);
}

void TestDefaultIdentity()
{
    const auto result = fo4hdr::ValidateRuntimeIdentity({});
    FO4HDR_CHECK(!result.valid);
    FO4HDR_CHECK(result.reason == fo4hdr::RuntimeValidationReason::RuntimeVersionMismatch);

    auto observed = supportedIdentity;
    observed.f4seVersion = {};
    const auto missingF4se = fo4hdr::ValidateRuntimeIdentity(observed);
    FO4HDR_CHECK(!missingF4se.valid);
    FO4HDR_CHECK(missingF4se.reason == fo4hdr::RuntimeValidationReason::F4seVersionMismatch);
}

void TestVersionComponents()
{
    for (std::size_t component = 0; component < supportedIdentity.runtimeVersion.size(); ++component)
    {
        auto observed = supportedIdentity;
        ++observed.runtimeVersion[component];
        const auto runtimeResult = fo4hdr::ValidateRuntimeIdentity(observed);
        FO4HDR_CHECK(!runtimeResult.valid);
        FO4HDR_CHECK(runtimeResult.reason == fo4hdr::RuntimeValidationReason::RuntimeVersionMismatch);

        observed = supportedIdentity;
        ++observed.f4seVersion[component];
        const auto f4seResult = fo4hdr::ValidateRuntimeIdentity(observed);
        FO4HDR_CHECK(!f4seResult.valid);
        FO4HDR_CHECK(f4seResult.reason == fo4hdr::RuntimeValidationReason::F4seVersionMismatch);
    }
}

void TestRuntimeMismatchPrecedence()
{
    auto observed = supportedIdentity;
    ++observed.runtimeVersion[0];
    ++observed.f4seVersion[0];
    const auto result = fo4hdr::ValidateRuntimeIdentity(observed);
    FO4HDR_CHECK(!result.valid);
    FO4HDR_CHECK(result.reason == fo4hdr::RuntimeValidationReason::RuntimeVersionMismatch);
}

}  // namespace

namespace fo4hdr::tests {

void RunRuntimeValidationTests()
{
    TestSupportedIdentity();
    TestDefaultIdentity();
    TestVersionComponents();
    TestRuntimeMismatchPrecedence();
}

}  // namespace fo4hdr::tests
