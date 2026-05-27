// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/runtime/RuntimeValidation.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace fo4hdr {

[[nodiscard]] std::optional<RuntimeVersion> DecodeFixedFileVersion(
    std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::optional<RuntimeVersion> ReadCurrentExecutableVersion() noexcept;

}  // namespace fo4hdr
