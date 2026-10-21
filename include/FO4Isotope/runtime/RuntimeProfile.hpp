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
    // Pinned build; project-owned native hooks install only on exactly this one.
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

enum class RuntimeSelectionReason
{
    Selected,
    UnsupportedRuntime
};

struct RuntimeSelection
{
    const RuntimeProfile* profile{};
    RuntimeSelectionReason reason{RuntimeSelectionReason::UnsupportedRuntime};
};

// Any packed executable version CommonLibF4 supports selects its OG, NG or AE
// family profile. ValidateRuntimeLoadGate then requires the Address Library.
[[nodiscard]] RuntimeSelection SelectRuntimeProfile(std::uint32_t runtimeVersion) noexcept;
[[nodiscard]] std::string_view ToString(RuntimeSelectionReason reason) noexcept;
[[nodiscard]] const RuntimeProfile* FindRuntimeProfile(std::uint32_t runtimeVersion) noexcept;
// Startup selects one immutable profile before publishing any callbacks.
[[nodiscard]] bool InitializeRuntimeProfile(std::uint32_t runtimeVersion) noexcept;
[[nodiscard]] const RuntimeProfile* ActiveRuntimeProfile() noexcept;
[[nodiscard]] bool IsOriginalRuntime() noexcept;
}  // namespace fo4hdr
