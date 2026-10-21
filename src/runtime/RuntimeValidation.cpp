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
};

constexpr IdentityCheck kIdentityChecks[]{
    {&RuntimeIdentity::runtimeVersion, RuntimeValidationReason::Valid,
     RuntimeValidationReason::RuntimeVersionMismatch},
    {&RuntimeIdentity::executableSha256, RuntimeValidationReason::ExecutableUnavailable,
     RuntimeValidationReason::ExecutableHashMismatch},
    {&RuntimeIdentity::f4seVersion, RuntimeValidationReason::Valid,
     RuntimeValidationReason::F4seVersionMismatch},
    {&RuntimeIdentity::f4seLoaderSha256, RuntimeValidationReason::F4seLoaderUnavailable,
     RuntimeValidationReason::F4seLoaderHashMismatch},
    {&RuntimeIdentity::f4seRuntimeSha256, RuntimeValidationReason::F4seRuntimeUnavailable,
     RuntimeValidationReason::F4seRuntimeHashMismatch},
    {&RuntimeIdentity::addressLibraryFile, RuntimeValidationReason::Valid,
     RuntimeValidationReason::AddressLibraryFileMismatch},
    {&RuntimeIdentity::addressLibrarySha256, RuntimeValidationReason::AddressLibraryUnavailable,
     RuntimeValidationReason::AddressLibraryHashMismatch}
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

}  // namespace

RuntimeValidationResult ValidateRuntimeIdentity(
    const RuntimeIdentity& expected,
    const RuntimeIdentity& observed) noexcept
{
    if (!IsInitialized(expected))
    {
        return Invalid(RuntimeValidationReason::ProfileUninitialized);
    }

    for (const auto& check : kIdentityChecks)
    {
        if (const auto reason = CheckField(check, expected, observed);
            reason != RuntimeValidationReason::Valid)
            return Invalid(reason);
    }

    return { true, RuntimeValidationReason::Valid };
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

}  // namespace fo4hdr
