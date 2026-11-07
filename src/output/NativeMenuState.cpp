// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/output/NativeMenuState.hpp"

#include <atomic>

namespace fo4hdr {
namespace {
// Generation in the upper bits, state in the low byte, so one CAS publishes both.
std::atomic<std::uint64_t> g_status{static_cast<std::uint8_t>(NativeMenuState::unknown)};
constexpr std::uint64_t kStateMask = 0xFFU;

[[nodiscard]] constexpr std::uint32_t Generation(std::uint64_t status) noexcept
{
    return static_cast<std::uint32_t>(status >> 8U);
}
}

NativeMenuState ReadNativeMenuState() noexcept
{
    return static_cast<NativeMenuState>(g_status.load(std::memory_order_acquire) & kStateMask);
}

std::uint32_t NativeMenuGeneration() noexcept
{
    return Generation(g_status.load(std::memory_order_acquire));
}

void ResetNativeMenuState() noexcept
{
    auto current = g_status.load(std::memory_order_acquire);
    for (;;)
    {
        auto generation = Generation(current) + 1U;
        if (generation == 0) generation = 1;
        const auto reset = static_cast<std::uint64_t>(generation) << 8U;
        if (g_status.compare_exchange_weak(
                current, reset, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
}

void PublishNativeMenuState(std::uint32_t generation, bool active) noexcept
{
    auto current = g_status.load(std::memory_order_acquire);
    const auto state = active ? NativeMenuState::active : NativeMenuState::inactive;
    const auto published = (static_cast<std::uint64_t>(generation) << 8U) |
        static_cast<std::uint8_t>(state);
    while (Generation(current) == generation)
    {
        if (g_status.compare_exchange_weak(
                current, published, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}

} // namespace fo4hdr
