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

// Exact comparison of compiled pinned identities; startup no longer observes
// installed files, so release validation owns the observed side.
[[nodiscard]] RuntimeValidationResult ValidateRuntimeIdentity(
    const RuntimeIdentity& expected,
    const RuntimeIdentity& observed) noexcept;

[[nodiscard]] std::string_view ToString(
    RuntimeValidationReason reason) noexcept;

}  // namespace fo4hdr
