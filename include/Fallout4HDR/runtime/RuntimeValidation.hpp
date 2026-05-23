// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace fo4hdr {

using RuntimeVersion = std::array<std::uint16_t, 4>;

struct RuntimeIdentity
{
    RuntimeVersion runtimeVersion{};
    RuntimeVersion f4seVersion{};
};

enum class RuntimeValidationReason
{
    Valid,
    RuntimeVersionMismatch,
    F4seVersionMismatch
};

struct RuntimeValidationResult
{
    bool valid;
    RuntimeValidationReason reason;
};

[[nodiscard]] RuntimeValidationResult ValidateRuntimeIdentity(
    const RuntimeIdentity& observed) noexcept;

}  // namespace fo4hdr
