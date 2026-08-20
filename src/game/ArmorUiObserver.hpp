// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "DirectCallPatch.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace fo4hdr::game {

struct ArmorUiCallSite
{
    std::uint64_t callerId{};
    std::uintptr_t callerRva{};
    std::uintptr_t callRva{};
    direct_call_patch::CallBytes bytes{};
};

// Supplied from the canonical first-party exact-runtime input, not discovered
// from executable scans. Byte agreement is necessary, not ABI/lifetime proof.
struct ArmorUiRuntimeFacts
{
    std::uint64_t targetId{};
    std::uintptr_t targetRva{};
    std::array<std::uint8_t, 23> targetBytes{};
    std::array<ArmorUiCallSite, 2> calls{};
};

struct ArmorUiInstallation
{
    ArmorUiRuntimeFacts facts;
    const void* trampolineInterface{};
    std::uint32_t pluginHandle{UINT32_MAX};
    // Trusted bootstrap admission, established before this call. Runtime
    // authorization does not follow from matching bytes or an F4SE load event.
    bool runtimeAuthorized{};
    bool preUseExclusive{};
    bool abiAndRegistryLifetimeProven{};
    bool addressLibraryAvailable{};
    bool trampolineOwned{};
};

// One admitted attempt, no per-frame installation. Refusal before admission
// leaves executable bytes and the Address Library loader untouched.
[[nodiscard]] bool InitializeArmorUiObserver(const ArmorUiInstallation& installation) noexcept;

}  // namespace fo4hdr::game
