// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/runtime/RuntimeValidation.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace fo4hdr {
namespace {

[[nodiscard]] bool IsLowerHexSha256(std::string_view value) noexcept
{
    return value.size() == 64 &&
        value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

[[nodiscard]] RuntimeValidationResult Invalid(
    RuntimeValidationReason reason) noexcept
{
    return { false, reason };
}

struct IdentityCheck
{
    std::string_view RuntimeIdentity::* field;
    RuntimeValidationReason unavailable;
    RuntimeValidationReason mismatch;
    // Address identity gates load; F4SE identity is audit evidence only.
    bool loadGate;
};

constexpr IdentityCheck kIdentityChecks[]{
    {&RuntimeIdentity::runtimeVersion, RuntimeValidationReason::Valid,
     RuntimeValidationReason::RuntimeVersionMismatch, true},
    {&RuntimeIdentity::executableSha256, RuntimeValidationReason::ExecutableUnavailable,
     RuntimeValidationReason::ExecutableHashMismatch, true},
    {&RuntimeIdentity::f4seVersion, RuntimeValidationReason::Valid,
     RuntimeValidationReason::F4seVersionMismatch, false},
    {&RuntimeIdentity::f4seLoaderSha256, RuntimeValidationReason::F4seLoaderUnavailable,
     RuntimeValidationReason::F4seLoaderHashMismatch, false},
    {&RuntimeIdentity::f4seRuntimeSha256, RuntimeValidationReason::F4seRuntimeUnavailable,
     RuntimeValidationReason::F4seRuntimeHashMismatch, false},
    {&RuntimeIdentity::addressLibraryFile, RuntimeValidationReason::Valid,
     RuntimeValidationReason::AddressLibraryFileMismatch, true},
    {&RuntimeIdentity::addressLibrarySha256, RuntimeValidationReason::AddressLibraryUnavailable,
     RuntimeValidationReason::AddressLibraryHashMismatch, true}
};

[[nodiscard]] bool IsInitialized(const RuntimeIdentity& identity) noexcept
{
    return std::all_of(std::begin(kIdentityChecks), std::end(kIdentityChecks),
        [&identity](const IdentityCheck& check) noexcept {
            const auto value = identity.*check.field;
            return check.unavailable == RuntimeValidationReason::Valid ?
                !value.empty() : IsLowerHexSha256(value);
        });
}

[[nodiscard]] RuntimeValidationReason CheckField(const IdentityCheck& check,
    const RuntimeIdentity& expected, const RuntimeIdentity& observed) noexcept
{
    const auto value = observed.*check.field;
    if (value.empty() && check.unavailable != RuntimeValidationReason::Valid)
        return check.unavailable;
    return value == expected.*check.field ? RuntimeValidationReason::Valid : check.mismatch;
}

[[nodiscard]] RuntimeValidationResult Validate(const RuntimeIdentity& expected,
    const RuntimeIdentity& observed, bool loadGateOnly) noexcept
{
    if (!IsInitialized(expected))
    {
        return Invalid(RuntimeValidationReason::ProfileUninitialized);
    }

    for (const auto& check : kIdentityChecks)
    {
        if (loadGateOnly && !check.loadGate) continue;
        if (const auto reason = CheckField(check, expected, observed);
            reason != RuntimeValidationReason::Valid)
            return Invalid(reason);
    }

    return { true, RuntimeValidationReason::Valid };
}

}  // namespace

RuntimeValidationResult ValidateRuntimeIdentity(
    const RuntimeIdentity& expected,
    const RuntimeIdentity& observed) noexcept
{
    return Validate(expected, observed, false);
}

RuntimeValidationResult ValidateRuntimeLoadGate(
    const RuntimeIdentity& expected,
    const RuntimeIdentity& observed) noexcept
{
    return Validate(expected, observed, true);
}

RuntimeActionResult ValidateRuntimeAction(
    std::string_view status,
    RuntimeAction action) noexcept
{
    constexpr std::pair<std::string_view, bool> statuses[]{
        {"runtime-pinned-unvalidated", true},
        {"capture-complete", true},
        {"release-approved", true},
        {"primary-uninitialized", false},
        {"planned", false}
    };
    const auto found = std::find_if(std::begin(statuses), std::end(statuses),
        [status](const auto& entry) noexcept { return entry.first == status; });
    if (found == std::end(statuses))
    {
        return { false, RuntimeActionReason::UnknownStatus };
    }

    bool allowed{};
    switch (action)
    {
    case RuntimeAction::MilestoneValidation:
        allowed = found->second;
        break;
    case RuntimeAction::ReleasePackaging:
        allowed = status == "release-approved";
        break;
    default:
        return { false, RuntimeActionReason::UnknownAction };
    }
    return {
        allowed,
        allowed ? RuntimeActionReason::Allowed :
                  RuntimeActionReason::StatusNotAuthorized
    };
}

std::string_view ToString(RuntimeValidationReason reason) noexcept
{
    constexpr std::pair<RuntimeValidationReason, std::string_view> names[]{
        {RuntimeValidationReason::Valid, "valid"},
        {RuntimeValidationReason::ProfileUninitialized, "profile-uninitialized"},
        {RuntimeValidationReason::RuntimeVersionMismatch, "runtime-version-mismatch"},
        {RuntimeValidationReason::ExecutableUnavailable, "executable-unavailable"},
        {RuntimeValidationReason::ExecutableHashMismatch, "executable-hash-mismatch"},
        {RuntimeValidationReason::F4seVersionMismatch, "f4se-version-mismatch"},
        {RuntimeValidationReason::F4seLoaderUnavailable, "f4se-loader-unavailable"},
        {RuntimeValidationReason::F4seLoaderHashMismatch, "f4se-loader-hash-mismatch"},
        {RuntimeValidationReason::F4seRuntimeUnavailable, "f4se-runtime-unavailable"},
        {RuntimeValidationReason::F4seRuntimeHashMismatch, "f4se-runtime-hash-mismatch"},
        {RuntimeValidationReason::AddressLibraryFileMismatch, "address-library-file-mismatch"},
        {RuntimeValidationReason::AddressLibraryUnavailable, "address-library-unavailable"},
        {RuntimeValidationReason::AddressLibraryHashMismatch, "address-library-hash-mismatch"}
    };
    const auto found = std::find_if(std::begin(names), std::end(names),
        [reason](const auto& entry) noexcept { return entry.first == reason; });
    return found == std::end(names) ? "unknown-validation-reason" : found->second;
}

std::string_view ToString(RuntimeAction action) noexcept
{
    switch (action)
    {
    case RuntimeAction::MilestoneValidation:
        return "milestone-validation";
    case RuntimeAction::ReleasePackaging:
        return "release-packaging";
    }
    return "unknown-runtime-action";
}

std::string_view ToString(RuntimeActionReason reason) noexcept
{
    switch (reason)
    {
    case RuntimeActionReason::Allowed:
        return "allowed";
    case RuntimeActionReason::UnknownStatus:
        return "unknown-status";
    case RuntimeActionReason::UnknownAction:
        return "unknown-action";
    case RuntimeActionReason::StatusNotAuthorized:
        return "status-not-authorized";
    }
    return "unknown-runtime-action-reason";
}

}  // namespace fo4hdr
