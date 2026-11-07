// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstdint>

namespace fo4hdr {

// Native pausing-menu observation for frame traces.
// Source invalidation rejects publication from its retired generation.
enum class NativeMenuState : std::uint8_t
{
    unknown,
    inactive,
    active
};

[[nodiscard]] NativeMenuState ReadNativeMenuState() noexcept;
[[nodiscard]] std::uint32_t NativeMenuGeneration() noexcept;
// Starts a new generation in the unknown state; generation zero is skipped.
void ResetNativeMenuState() noexcept;
// Publishes only while generation is still current.
void PublishNativeMenuState(std::uint32_t generation, bool active) noexcept;

} // namespace fo4hdr
