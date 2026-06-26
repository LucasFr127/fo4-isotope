// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/ui/HdrSettingsInput.hpp"

namespace fo4hdr {
namespace {

bool IsKeyDown(UINT message) noexcept
{
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
}

bool IsKeyUp(UINT message) noexcept
{
    return message == WM_KEYUP || message == WM_SYSKEYUP;
}

}  // namespace

bool HdrSettingsInput::Initialize(HWND window) noexcept
{
    if (window == nullptr || window_.load(std::memory_order_acquire) != nullptr) return false;
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (thread != GetCurrentThreadId() || process != GetCurrentProcessId()) return false;
    ClearQueue();
    ownerThread_.store(thread, std::memory_order_release);
    window_.store(window, std::memory_order_release);
    return true;
}

bool HdrSettingsInput::OnOwnerThread() const noexcept
{
    return ownerThread_.load(std::memory_order_acquire) == GetCurrentThreadId();
}

void HdrSettingsInput::ClearQueue() noexcept
{
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    armed_ = false;
    resetRequested_.store(false, std::memory_order_release);
}

bool HdrSettingsInput::Stop() noexcept
{
    if (!OnOwnerThread()) return false;
    window_.store(nullptr, std::memory_order_release);
    ClearQueue();
    return true;
}

void HdrSettingsInput::Push(Edge edge) noexcept
{
    if (resetRequested_.load(std::memory_order_acquire)) return;
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % capacity;
    if (next == read_.load(std::memory_order_acquire))
    {
        resetRequested_.store(true, std::memory_order_release);
        return;
    }
    edges_[write] = edge;
    write_.store(next, std::memory_order_release);
}

void HdrSettingsInput::Observe(HWND window, UINT message, WPARAM key, LPARAM details) noexcept
{
    if (window == nullptr) return;
    if (!OnOwnerThread() || window != window_.load(std::memory_order_acquire)) return;
    if (message == WM_KILLFOCUS)
    {
        resetRequested_.store(true, std::memory_order_release);
        return;
    }
    if (key != VK_F10) return;
    ObserveToggleKey(message, details);
}

void HdrSettingsInput::ObserveToggleKey(UINT message, LPARAM details) noexcept
{
    if (IsKeyUp(message))
    {
        Push(Edge::up);
        return;
    }
    const auto initialDown = (static_cast<std::uintptr_t>(details) & (1U << 30)) == 0;
    if (IsKeyDown(message) && initialDown) Push(Edge::down);
}

void HdrSettingsInput::ApplyEdge(Edge edge, HdrSettingsInputSnapshot& snapshot) noexcept
{
    if (edge == Edge::down)
    {
        armed_ = true;
        return;
    }
    if (armed_) ++snapshot.toggles;
    armed_ = false;
}

HdrSettingsInputSnapshot HdrSettingsInput::Drain() noexcept
{
    HdrSettingsInputSnapshot snapshot;
    if (!OnOwnerThread() || window_.load(std::memory_order_acquire) == nullptr) return snapshot;
    if (resetRequested_.load(std::memory_order_acquire))
    {
        // The producer stops enqueueing until the consumer resets its cursor and
        // latch, then acknowledges. Overflow follows the same conservative clear.
        ClearQueue();
        snapshot.focusLost = true;
        return snapshot;
    }
    auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    while (read != write)
    {
        ApplyEdge(edges_[read], snapshot);
        read = (read + 1) % capacity;
    }
    read_.store(read, std::memory_order_release);
    return snapshot;
}

}  // namespace fo4hdr
