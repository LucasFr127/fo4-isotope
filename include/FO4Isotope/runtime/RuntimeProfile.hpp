// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "RuntimeValidation.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fo4hdr {
struct NativeUiRuntimeProfile
{
    std::uintptr_t targetRva{};
    std::uint64_t targetId{};
    std::array<std::uint8_t, 16> targetBytes{};
    std::array<std::uintptr_t, 2> callerRvas{};
    std::array<std::uint64_t, 2> callerIds{};
    std::array<std::uintptr_t, 2> callRvas{};
    std::array<std::array<std::uint8_t, 5>, 2> callBytes{};
};

struct ImageSpaceRuntimeProfile
{
    std::array<std::uintptr_t, 4> targetRvas{};
    std::array<std::uint64_t, 4> targetIds{};
    std::array<std::span<const std::uint8_t>, 4> targetBytes{};
    std::uintptr_t globalLutRva{}, globalBaseRva{};
    std::uint64_t globalLutId{}, globalBaseId{};
    std::array<std::uintptr_t, 11> callRvas{}, callerRvas{};
    std::array<std::uint64_t, 11> callerIds{};
    std::array<std::uint32_t, 11> callTargetIndices{};
    std::array<std::array<std::uint8_t, 5>, 11> callBytes{};
    std::size_t sunlightColorIndex{}, glareColorIndex{};
    std::array<std::uint8_t, 3> colorShifts{};
};

struct RuntimeProfile
{
    std::string_view key;
    RuntimeIdentity identity;
    std::string_view manifestSha256;
    std::string_view status;
    std::string_view f4seRuntimeFile;
    std::string_view commonLibCommit;
    // Pinned build whose evidence produced the profile's facts.
    std::uint32_t runtimeVersion{};
    // Pinned F4SE release; audit evidence, not a load requirement.
    std::uint32_t f4seVersion{};
    bool hasNativeSceneComposite{};
    std::array<std::uint32_t, 4> nativeSceneCompositeHash{};
    std::array<std::uint32_t, 4> lateGameUiCompositeHash{};
    std::array<std::uint32_t, 4> lateGameUiBackdropHash{};
    const NativeUiRuntimeProfile* nativeUi{};
    const ImageSpaceRuntimeProfile* imageSpace{};
};

// CommonLibF4's runtime families; every build it supports shares one profile.
enum class RuntimeFamily : std::uint8_t
{
    og,
    ng,
    ae
};

[[nodiscard]] const RuntimeProfile* FindRuntimeProfile(RuntimeFamily family) noexcept;
// Startup selects one immutable profile before publishing any callbacks.
[[nodiscard]] bool InitializeRuntimeProfile(RuntimeFamily family) noexcept;
[[nodiscard]] const RuntimeProfile* ActiveRuntimeProfile() noexcept;
[[nodiscard]] bool IsOriginalRuntime() noexcept;
}  // namespace fo4hdr
