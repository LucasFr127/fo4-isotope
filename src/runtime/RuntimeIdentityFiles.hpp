// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace fo4hdr {

[[nodiscard]] std::optional<std::string> Sha256File(
    const std::filesystem::path& path) noexcept;

[[nodiscard]] std::optional<std::string> Sha256CanonicalTextFile(
    const std::filesystem::path& path) noexcept;

}  // namespace fo4hdr
