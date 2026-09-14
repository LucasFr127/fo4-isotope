// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include <cstddef>
#include <cstdint>
#include <array>

namespace RE { class ImageSpaceManager; }

namespace fo4hdr::game {

struct NativeLutRuntimeFacts
{
    std::uint64_t targetId{};
    std::uintptr_t targetRva{};
    std::array<std::uint8_t, 25> targetBytes{};
    std::uint32_t effectSubobjectOffset{}, textureSrvOffset{};
};

struct NativeLutInstallation
{
    NativeLutRuntimeFacts facts;
    // Matching code/table bytes do not supply these trusted caller facts.
    bool runtimeAuthorized{}, preUseExclusive{}, abiAndRegistryLifetimeProven{};
    bool addressLibraryAvailable{};

    [[nodiscard]] bool Admitted() const noexcept
    {
        return runtimeAuthorized && preUseExclusive && abiAndRegistryLifetimeProven &&
            addressLibraryAvailable;
    }
};

[[nodiscard]] bool InitializeNativeLutObserver(const NativeLutInstallation& installation) noexcept;

// The admitted synchronous Gamma Render caller owns manager/texture lifetimes.
// The opaque SRV offset is a reviewed exact-runtime fact, not inferred from RE.
[[nodiscard]] bool ObserveNativeLuts(const RE::ImageSpaceManager* manager,
    std::uint64_t resourceGeneration, std::size_t textureSrvOffset,
    NativeLutObservation& destination) noexcept;

}  // namespace fo4hdr::game
