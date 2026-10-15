// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "RuntimeValidation.hpp"
#include <array>
#include <cstdint>
#include <string_view>

namespace fo4hdr {
struct RuntimeProfile
{
    std::string_view key;
    RuntimeIdentity identity;
    std::string_view status;
    std::string_view f4seRuntimeFile;
    std::string_view commonLibCommit;
    std::uint32_t runtimeVersion{};
    std::uint32_t f4seVersion{};
    bool hasNativeSceneComposite{};
    std::array<std::uint32_t, 4> nativeSceneCompositeHash{};
    std::array<std::uint32_t, 4> lateGameUiCompositeHash{};
    std::array<std::uint32_t, 4> lateGameUiBackdropHash{};
};

[[nodiscard]] const RuntimeProfile* FindRuntimeProfile(
    std::uint32_t runtimeVersion, std::uint32_t f4seVersion) noexcept;
// Startup selects one immutable profile before publishing any callbacks.
[[nodiscard]] bool InitializeRuntimeProfile(
    std::uint32_t runtimeVersion, std::uint32_t f4seVersion) noexcept;
[[nodiscard]] const RuntimeProfile* ActiveRuntimeProfile() noexcept;
[[nodiscard]] bool IsOriginalRuntime() noexcept;
}  // namespace fo4hdr
