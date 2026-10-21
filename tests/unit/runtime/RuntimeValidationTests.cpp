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
    const auto* og = FindRuntimeProfile(0x010A0A30, 0x00060170);
    const auto* ae = FindRuntimeProfile(0x010B0F00, 0x00070090);
    FO4HDR_CHECK(og && ae && og != ae);
    FO4HDR_CHECK(og->key == "1.10.163" && ae->key == "1.11.240");
    FO4HDR_CHECK(og->hasNativeSceneComposite && ae->hasNativeSceneComposite);
    FO4HDR_CHECK(og->nativeSceneCompositeHash != ae->nativeSceneCompositeHash);
    FO4HDR_CHECK(og->identity.executableSha256 != ae->identity.executableSha256);
    FO4HDR_CHECK(ValidateRuntimeIdentity(og->identity, og->identity).valid);
    FO4HDR_CHECK(ValidateRuntimeIdentity(ae->identity, ae->identity).valid);
    FO4HDR_CHECK(!ValidateRuntimeIdentity(og->identity, ae->identity).valid);
    FO4HDR_CHECK(FindRuntimeProfile(0x010A0A30, 0x00070090) == og);
    FO4HDR_CHECK(!FindRuntimeProfile(0x010B0F00, 0x00060170));
    FO4HDR_CHECK(!FindRuntimeProfile(0x010B0F00, 0x00070020));
    FO4HDR_CHECK(!FindRuntimeProfile(0x010B0DD0, 0x00070080));
    FO4HDR_CHECK(!InitializeRuntimeProfile(0, 0));
    FO4HDR_CHECK(InitializeRuntimeProfile(0x010A0A30, 0x00060170));
    FO4HDR_CHECK(InitializeRuntimeProfile(0x010A0A30, 0x00060170));
    FO4HDR_CHECK(!InitializeRuntimeProfile(0x010B0F00, 0x00070090));
    FO4HDR_CHECK(ActiveRuntimeProfile() == og && IsOriginalRuntime());
}

void TestNextGenerationRuntimeSelection()
{
    const auto* ng = FindRuntimeProfile(0x010A3D80, 0x00070020);
    FO4HDR_CHECK(ng);
    if (!ng) return;
    FO4HDR_CHECK(ng->key == "1.10.984");
    FO4HDR_CHECK(ng->hasNativeSceneComposite);
    FO4HDR_CHECK(ValidateRuntimeIdentity(ng->identity, ng->identity).valid);
    const auto* og = FindRuntimeProfile(0x010A0A30, 0x00060170);
    FO4HDR_CHECK(!ValidateRuntimeIdentity(og->identity, ng->identity).valid);
    FO4HDR_CHECK(FindRuntimeProfile(0x010A3D80, 0x00070090) == ng);
}

void TestRuntimeSelectionGate()
{
    const auto* ae = FindRuntimeProfile(0x010B0F00, 0x00070090);
    // A newer F4SE point release for the exact runtime is admitted.
    auto selection = SelectRuntimeProfile(0x010B0F00, 0x000700A0);
    FO4HDR_CHECK(selection.reason == RuntimeSelectionReason::Selected && selection.profile == ae);
    // An older F4SE names the exact-runtime candidate but is rejected.
    selection = SelectRuntimeProfile(0x010B0F00, 0x00070080);
    FO4HDR_CHECK(selection.reason == RuntimeSelectionReason::F4seBelowMinimum && selection.profile == ae);
    FO4HDR_CHECK(!FindRuntimeProfile(0x010B0F00, 0x00070080));
    // Other AE executables and FO4VR 1.2.72 never borrow a compiled profile.
    for (const auto runtime : {0x010B0A90U, 0x010B0BF0U, 0x010B0DD0U, 0x010B0F10U, 0x01020480U})
    {
        selection = SelectRuntimeProfile(runtime, 0x00070090);
        FO4HDR_CHECK(selection.reason == RuntimeSelectionReason::UnsupportedRuntime && !selection.profile);
    }
    FO4HDR_CHECK(ToString(RuntimeSelectionReason::Selected) == "selected");
    FO4HDR_CHECK(ToString(RuntimeSelectionReason::UnsupportedRuntime) == "unsupported-runtime");
    FO4HDR_CHECK(ToString(RuntimeSelectionReason::F4seBelowMinimum) == "f4se-below-minimum");
    FO4HDR_CHECK(ToString(static_cast<RuntimeSelectionReason>(-1)) == "unknown-selection-reason");
}

void TestCompiledRuntimeProvenance()
{
    for (const auto* profile : {
             FindRuntimeProfile(0x010A0A30, 0x00060170),
             FindRuntimeProfile(0x010A3D80, 0x00070020),
             FindRuntimeProfile(0x010B0F00, 0x00070090)})
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

void TestLoadGate()
{
    // F4SE identity is audit evidence: point releases and missing loaders load.
    auto observed = expected;
    observed.f4seVersion = "0.6.24";
    observed.f4seRuntimeSha256 = hashA;
    observed.f4seLoaderSha256 = {};
    FO4HDR_CHECK(ValidateRuntimeLoadGate(expected, observed).valid);
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, observed).reason ==
        RuntimeValidationReason::F4seVersionMismatch);

    // A re-released Address Library loads; only the exact audit rejects it.
    auto rereleased = expected;
    rereleased.addressLibrarySha256 = hashA;
    FO4HDR_CHECK(ValidateRuntimeLoadGate(expected, rereleased).valid);
    FO4HDR_CHECK(ValidateRuntimeIdentity(expected, rereleased).reason ==
        RuntimeValidationReason::AddressLibraryHashMismatch);

    // The executable is exact address identity; the Address Library must exist.
    const struct
    {
        std::string_view RuntimeIdentity::* field;
        std::string_view value;
        RuntimeValidationReason reason;
    } rejected[]{
        {&RuntimeIdentity::runtimeVersion, "1.10.984.0", RuntimeValidationReason::RuntimeVersionMismatch},
        {&RuntimeIdentity::executableSha256, hashB, RuntimeValidationReason::ExecutableHashMismatch},
        {&RuntimeIdentity::executableSha256, {}, RuntimeValidationReason::ExecutableUnavailable},
        {&RuntimeIdentity::addressLibrarySha256, {}, RuntimeValidationReason::AddressLibraryUnavailable},
        {&RuntimeIdentity::addressLibraryFile, "version-1-10-984-0.bin",
            RuntimeValidationReason::AddressLibraryFileMismatch}};
    for (const auto& entry : rejected)
    {
        auto candidate = observed;
        candidate.*entry.field = entry.value;
        const auto result = ValidateRuntimeLoadGate(expected, candidate);
        FO4HDR_CHECK(!result.valid && result.reason == entry.reason);
    }

    auto uninitialized = expected;
    uninitialized.f4seLoaderSha256 = "UNVALIDATED";
    FO4HDR_CHECK(ValidateRuntimeLoadGate(uninitialized, expected).reason ==
        RuntimeValidationReason::ProfileUninitialized);
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

void TestMilestoneAuthorization()
{
    for (const auto status : {
             std::string_view{"runtime-pinned-unvalidated"},
             std::string_view{"capture-complete"},
             std::string_view{"release-approved"} })
    {
        const auto authorization = fo4hdr::ValidateRuntimeAction(
            status,
            fo4hdr::RuntimeAction::MilestoneValidation);
        FO4HDR_CHECK(authorization.allowed);
        FO4HDR_CHECK(
            authorization.reason == fo4hdr::RuntimeActionReason::Allowed);
    }
}

void TestReleaseAuthorization()
{
    for (const auto status : {
             std::string_view{"primary-uninitialized"},
             std::string_view{"planned"},
             std::string_view{"runtime-pinned-unvalidated"},
             std::string_view{"capture-complete"} })
    {
        const auto authorization = fo4hdr::ValidateRuntimeAction(
            status,
            fo4hdr::RuntimeAction::ReleasePackaging);
        FO4HDR_CHECK(!authorization.allowed);
        FO4HDR_CHECK(
            authorization.reason ==
            fo4hdr::RuntimeActionReason::StatusNotAuthorized);
    }
    FO4HDR_CHECK(fo4hdr::ValidateRuntimeAction(
        "release-approved",
        fo4hdr::RuntimeAction::ReleasePackaging).allowed);
}

void TestUnknownAuthorization()
{
    const auto unknownAuthorization = fo4hdr::ValidateRuntimeAction(
        "captured",
        fo4hdr::RuntimeAction::MilestoneValidation);
    FO4HDR_CHECK(!unknownAuthorization.allowed);
    FO4HDR_CHECK(
        unknownAuthorization.reason ==
        fo4hdr::RuntimeActionReason::UnknownStatus);
    FO4HDR_CHECK(
        fo4hdr::ToString(fo4hdr::RuntimeAction::MilestoneValidation) ==
        "milestone-validation");
    FO4HDR_CHECK(
        fo4hdr::ToString(
            fo4hdr::RuntimeActionReason::StatusNotAuthorized) ==
        "status-not-authorized");
    const auto unknownAction = fo4hdr::ValidateRuntimeAction(
        "release-approved",
        static_cast<fo4hdr::RuntimeAction>(0xFF));
    FO4HDR_CHECK(!unknownAction.allowed);
    FO4HDR_CHECK(
        unknownAction.reason == fo4hdr::RuntimeActionReason::UnknownAction);
}
}  // namespace

void RunRuntimeValidationTests()
{
    TestSharedRuntimeSelection();
    TestNextGenerationRuntimeSelection();
    TestRuntimeSelectionGate();
    TestCompiledRuntimeProvenance();
    TestMatchingIdentity();
    TestExecutableIdentity();
    TestRuntimeAndF4seIdentity();
    TestAddressLibraryIdentity();
    TestLoadGate();
    TestUninitializedProfile();
    TestFailurePrecedence();
    TestReasonNames();
    TestMilestoneAuthorization();
    TestReleaseAuthorization();
    TestUnknownAuthorization();
}
}  // namespace fo4hdr::tests
