// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "TestHarness.hpp"

#include "FO4Isotope/runtime/RuntimeValidation.hpp"
#include "FO4Isotope/runtime/RuntimeProfile.hpp"

#include <string_view>

namespace fo4hdr::tests {

namespace {
void TestSharedRuntimeSelection()
{
    const auto* og = FindRuntimeProfile(RuntimeFamily::og);
    const auto* ae = FindRuntimeProfile(RuntimeFamily::ae);
    FO4HDR_CHECK(og && ae && og != ae);
    FO4HDR_CHECK(og->key == "1.10.163" && ae->key == "1.11.240");
    FO4HDR_CHECK(og->hasNativeSceneComposite && ae->hasNativeSceneComposite);
    FO4HDR_CHECK(og->nativeSceneCompositeHash != ae->nativeSceneCompositeHash);
    FO4HDR_CHECK(og->identity.executableSha256 != ae->identity.executableSha256);
    FO4HDR_CHECK(ValidateRuntimeIdentity(og->identity, og->identity).valid);
    FO4HDR_CHECK(ValidateRuntimeIdentity(ae->identity, ae->identity).valid);
    FO4HDR_CHECK(!ValidateRuntimeIdentity(og->identity, ae->identity).valid);
    FO4HDR_CHECK(!FindRuntimeProfile(static_cast<RuntimeFamily>(3)));
    FO4HDR_CHECK(!InitializeRuntimeProfile(static_cast<RuntimeFamily>(3)));
    FO4HDR_CHECK(InitializeRuntimeProfile(RuntimeFamily::og));
    FO4HDR_CHECK(InitializeRuntimeProfile(RuntimeFamily::og));
    FO4HDR_CHECK(!InitializeRuntimeProfile(RuntimeFamily::ae));
    FO4HDR_CHECK(ActiveRuntimeProfile() == og && IsOriginalRuntime());
}

void TestNextGenerationRuntimeSelection()
{
    const auto* ng = FindRuntimeProfile(RuntimeFamily::ng);
    FO4HDR_CHECK(ng);
    if (!ng) return;
    FO4HDR_CHECK(ng->key == "1.10.984");
    FO4HDR_CHECK(ng->hasNativeSceneComposite);
    FO4HDR_CHECK(ValidateRuntimeIdentity(ng->identity, ng->identity).valid);
    const auto* og = FindRuntimeProfile(RuntimeFamily::og);
    FO4HDR_CHECK(!ValidateRuntimeIdentity(og->identity, ng->identity).valid);
}

void TestCompiledRuntimeProvenance()
{
    for (const auto* profile : {
             FindRuntimeProfile(RuntimeFamily::og),
             FindRuntimeProfile(RuntimeFamily::ng),
             FindRuntimeProfile(RuntimeFamily::ae)})
    {
        FO4HDR_CHECK(profile);
        if (!profile) continue;
        FO4HDR_CHECK(profile->manifestSha256.size() == 64);
        FO4HDR_CHECK(profile->manifestSha256.find_first_not_of("0123456789abcdef") ==
            std::string_view::npos);
        FO4HDR_CHECK(profile->nativeUi && profile->imageSpace);
    }
}

constexpr std::string_view hashA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view hashB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view hashC =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view hashD =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

const fo4hdr::RuntimeIdentity expected{
    .runtimeVersion = "1.10.163.0",
    .executableSha256 = hashA,
    .f4seVersion = "0.6.23",
    .f4seLoaderSha256 = hashB,
    .f4seRuntimeSha256 = hashC,
    .addressLibraryFile = "version-1-10-163-0.bin",
    .addressLibrarySha256 = hashD
};

void TestMatchingIdentity()
{
    auto observed = expected;
    auto result =
        fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(result.valid);
    FO4HDR_CHECK(
        result.reason == fo4hdr::RuntimeValidationReason::Valid);
}

void TestExecutableIdentity()
{
    auto observed = expected;
    RuntimeValidationResult result;
    observed.executableSha256 = hashB;
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(!result.valid);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::ExecutableHashMismatch);
    FO4HDR_CHECK(
        fo4hdr::ToString(result.reason) ==
        "executable-hash-mismatch");

    observed = expected;
    observed.executableSha256 = {};
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::ExecutableUnavailable);
}

void TestRuntimeAndF4seIdentity()
{
    auto observed = expected;
    RuntimeValidationResult result;
    observed.runtimeVersion = "1.10.984.0";
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::RuntimeVersionMismatch);

    observed = expected;
    observed.f4seVersion = "0.7.2";
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::F4seVersionMismatch);

    observed = expected;
    observed.f4seLoaderSha256 = hashA;
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::F4seLoaderHashMismatch);

    observed = expected;
    observed.f4seLoaderSha256 = {};
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::F4seLoaderUnavailable);

    observed = expected;
    observed.f4seRuntimeSha256 = hashA;
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::F4seRuntimeHashMismatch);

    observed = expected;
    observed.f4seRuntimeSha256 = {};
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::F4seRuntimeUnavailable);
}

void TestAddressLibraryIdentity()
{
    auto observed = expected;
    RuntimeValidationResult result;
    observed.addressLibraryFile = "version-1-10-984-0.bin";
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::AddressLibraryFileMismatch);

    observed = expected;
    observed.addressLibrarySha256 = {};
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(!result.valid);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::AddressLibraryUnavailable);

    observed = expected;
    observed.addressLibrarySha256 = hashA;
    result = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::AddressLibraryHashMismatch);
}

void TestUninitializedProfile()
{
    RuntimeValidationResult result;
    auto uninitialized = expected;
    uninitialized.executableSha256 = "UNVALIDATED";
    result = fo4hdr::ValidateRuntimeIdentity(uninitialized, expected);
    FO4HDR_CHECK(!result.valid);
    FO4HDR_CHECK(
        result.reason ==
        fo4hdr::RuntimeValidationReason::ProfileUninitialized);
}

void TestFailurePrecedence()
{
    auto observed = expected;
    observed.runtimeVersion = "unknown";
    observed.executableSha256 = {};
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, observed).reason ==
        RuntimeValidationReason::RuntimeVersionMismatch);
    observed.runtimeVersion = expected.runtimeVersion;
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, observed).reason ==
        RuntimeValidationReason::ExecutableUnavailable);
    auto uninitialized = expected;
    uninitialized.addressLibrarySha256 = "UNVALIDATED";
    FO4HDR_CHECK(ValidateRuntimeIdentity(uninitialized, observed).reason ==
        RuntimeValidationReason::ProfileUninitialized);
    FO4HDR_CHECK(ToString(static_cast<RuntimeValidationReason>(-1)) ==
        "unknown-validation-reason");
}

struct ReasonText
{
    fo4hdr::RuntimeValidationReason reason;
    std::string_view text;
};
constexpr ReasonText reasonTexts[]{
    { fo4hdr::RuntimeValidationReason::Valid, "valid" },
    { fo4hdr::RuntimeValidationReason::ProfileUninitialized,
      "profile-uninitialized" },
    { fo4hdr::RuntimeValidationReason::RuntimeVersionMismatch,
      "runtime-version-mismatch" },
    { fo4hdr::RuntimeValidationReason::ExecutableUnavailable,
      "executable-unavailable" },
    { fo4hdr::RuntimeValidationReason::ExecutableHashMismatch,
      "executable-hash-mismatch" },
    { fo4hdr::RuntimeValidationReason::F4seVersionMismatch,
      "f4se-version-mismatch" },
    { fo4hdr::RuntimeValidationReason::F4seLoaderUnavailable,
      "f4se-loader-unavailable" },
    { fo4hdr::RuntimeValidationReason::F4seLoaderHashMismatch,
      "f4se-loader-hash-mismatch" },
    { fo4hdr::RuntimeValidationReason::F4seRuntimeUnavailable,
      "f4se-runtime-unavailable" },
    { fo4hdr::RuntimeValidationReason::F4seRuntimeHashMismatch,
      "f4se-runtime-hash-mismatch" },
    { fo4hdr::RuntimeValidationReason::AddressLibraryFileMismatch,
      "address-library-file-mismatch" },
    { fo4hdr::RuntimeValidationReason::AddressLibraryUnavailable,
      "address-library-unavailable" },
    { fo4hdr::RuntimeValidationReason::AddressLibraryHashMismatch,
      "address-library-hash-mismatch" }
};
void TestReasonNames()
{
    for (const auto& reasonText : reasonTexts)
    {
        FO4HDR_CHECK(
            fo4hdr::ToString(reasonText.reason) == reasonText.text);
    }
}

}  // namespace

void RunRuntimeValidationTests()
{
    TestSharedRuntimeSelection();
    TestNextGenerationRuntimeSelection();
    TestCompiledRuntimeProvenance();
    TestMatchingIdentity();
    TestExecutableIdentity();
    TestRuntimeAndF4seIdentity();
    TestAddressLibraryIdentity();
    TestUninitializedProfile();
    TestFailurePrecedence();
    TestReasonNames();
}
}  // namespace fo4hdr::tests
