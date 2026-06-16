// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dxgi.h>

#include <cstdint>
#include <span>

namespace fo4hdr {

struct SwapChainObservation
{
    std::uint64_t generation{};
    bool active{};
};

// Only an exclusive fixture setup may supply its unique complete base table.
// Object/table storage and nonthrowing predecessor code survive all callbacks.
// Never supply a driver table or call this during published/in-flight fixture work.
[[nodiscard]] bool SelectOwnedSwapChain(IDXGISwapChain* chain,
    std::span<void*, 18> table, HWND window) noexcept;
void OfferCreatedSwapChain(IDXGISwapChain* chain) noexcept;
[[nodiscard]] SwapChainObservation ReadSwapChainObservation() noexcept;

}  // namespace fo4hdr
