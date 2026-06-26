// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

struct HdrSettingsInputSnapshot
{
    std::uint32_t toggles{};
    bool focusLost{};
};

// One fixed window producer and one owner-thread consumer. This observes F10
// edges only; it neither installs a window procedure nor consumes native input.
class HdrSettingsInput
{
public:
    // Cold setup and Stop require the window owner, with no callback in flight.
    [[nodiscard]] bool Initialize(HWND window) noexcept;
    [[nodiscard]] bool Stop() noexcept;
    void Observe(HWND window, UINT message, WPARAM key, LPARAM details) noexcept;
    [[nodiscard]] HdrSettingsInputSnapshot Drain() noexcept;

private:
    enum class Edge : std::uint8_t { down, up };
    static constexpr std::size_t capacity = 64;

    [[nodiscard]] bool OnOwnerThread() const noexcept;
    void Push(Edge edge) noexcept;
    void ObserveToggleKey(UINT message, LPARAM details) noexcept;
    void ClearQueue() noexcept;
    void ApplyEdge(Edge edge, HdrSettingsInputSnapshot& snapshot) noexcept;

    std::array<Edge, capacity> edges_{};
    std::atomic<std::size_t> write_{};
    std::atomic<std::size_t> read_{};
    std::atomic<bool> resetRequested_{};
    std::atomic<HWND> window_{};
    std::atomic<DWORD> ownerThread_{};
    bool armed_{};
};

}  // namespace fo4hdr
