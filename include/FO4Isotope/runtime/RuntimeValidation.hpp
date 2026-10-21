// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <string_view>

namespace fo4hdr {

struct RuntimeIdentity
{
    std::string_view runtimeVersion;
    std::string_view executableSha256;
    std::string_view f4seVersion;
    std::string_view f4seLoaderSha256;
    std::string_view f4seRuntimeSha256;
    std::string_view addressLibraryFile;
    std::string_view addressLibrarySha256;
};

enum class RuntimeValidationReason
{
    Valid,
    ProfileUninitialized,
    RuntimeVersionMismatch,
    ExecutableUnavailable,
    ExecutableHashMismatch,
    F4seVersionMismatch,
    F4seLoaderUnavailable,
    F4seLoaderHashMismatch,
    F4seRuntimeUnavailable,
    F4seRuntimeHashMismatch,
    AddressLibraryFileMismatch,
    AddressLibraryUnavailable,
    AddressLibraryHashMismatch
};

struct RuntimeValidationResult
{
    bool valid;
    RuntimeValidationReason reason;
};

enum class RuntimeAction
{
    MilestoneValidation,
    ReleasePackaging
};

enum class RuntimeActionReason
{
    Allowed,
    UnknownStatus,
    UnknownAction,
    StatusNotAuthorized
};

struct RuntimeActionResult
{
    bool allowed;
    RuntimeActionReason reason;
};

// Exact audit of every pinned identity, required for release evidence.
[[nodiscard]] RuntimeValidationResult ValidateRuntimeIdentity(
    const RuntimeIdentity& expected,
    const RuntimeIdentity& observed) noexcept;

// Startup gate: executable version and hash, and Address Library file presence
// only. Address Library and F4SE hashes and the F4SE version are excluded;
// selection enforces the minimum F4SE release.
[[nodiscard]] RuntimeValidationResult ValidateRuntimeLoadGate(
    const RuntimeIdentity& expected,
    const RuntimeIdentity& observed) noexcept;

[[nodiscard]] std::string_view ToString(
    RuntimeValidationReason reason) noexcept;

[[nodiscard]] RuntimeActionResult ValidateRuntimeAction(
    std::string_view status,
    RuntimeAction action) noexcept;

[[nodiscard]] std::string_view ToString(
    RuntimeAction action) noexcept;

[[nodiscard]] std::string_view ToString(
    RuntimeActionReason reason) noexcept;

}  // namespace fo4hdr
