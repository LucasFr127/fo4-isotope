// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <algorithm>


#include "FO4Isotope/config/HdrSettings.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <array>

namespace fo4hdr {

using HdrSettingsInteractionRequestHandler = bool (*)(
    bool open, std::uint32_t windowThreadId) noexcept;

enum class HdrSettingsInteractionState : std::uint8_t
{
    unavailable,
    inactive,
    opening,
    active,
    closing,
    failed
};

// Native pausing-menu observation is independent of settings activation.
// Source invalidation rejects publication from its retired generation.
enum class HdrSettingsNativeMenuState : std::uint8_t
{
    unknown,
    inactive,
    active
};

[[nodiscard]] HdrSettingsNativeMenuState ReadHdrSettingsNativeMenuState() noexcept;
[[nodiscard]] std::uint32_t HdrSettingsNativeMenuGeneration() noexcept;
void ResetHdrSettingsNativeMenuState() noexcept;
void PublishHdrSettingsNativeMenuState(std::uint32_t generation, bool active) noexcept;

// Plain-data bridge between the graphics-owned interface and the game adapter.
// The Win32 input boundary invokes the handler only from the verified owner of
// the Fallout HWND; callers on other threads are marshalled through that
// window's message queue.
void RegisterHdrSettingsInteractionRequestHandler(
    HdrSettingsInteractionRequestHandler handler) noexcept;
[[nodiscard]] bool RequestHdrSettingsInteraction(bool open) noexcept;
[[nodiscard]] HdrSettingsInteractionState
ReadHdrSettingsInteractionState() noexcept;


// Window-thread-owned latch: exact modifiers are checked on the initial down,
// then releasing the base key completes the chord even if modifiers went up
// first. Focus loss/rebinding must reset the latch. Repeats never arm it.
struct HdrSettingsShortcutLatch
{
    std::uint32_t pendingKey{};

    [[nodiscard]] constexpr bool KeyDown(
        HdrSettingsHotkey configured, std::uint32_t key,
        std::uint32_t modifiers, bool repeat) noexcept
    {
        const HdrSettingsHotkey pressed{key, modifiers};
        if (repeat || pressed != configured) return false;
        pendingKey = key;
        return true;
    }

    [[nodiscard]] constexpr bool KeyUp(std::uint32_t key) noexcept
    {
        if (pendingKey == 0 || key != pendingKey) return false;
        pendingKey = 0;
        return true;
    }

    constexpr void Reset() noexcept { pendingKey = 0; }
};

struct HdrSettingsCaptureState
{
    bool capturedDown{};
    bool active{};
};

// Keep shortcut capture out of ImGui navigation/text as well as the toggle
// path. Translated characters can arrive after keyup; their scan marker is
// therefore retained until the next initial down of that physical key.
struct HdrSettingsCapturedGestureFilter
{
    std::array<bool, 256> keys{};
    std::array<bool, 512> scans{};

    [[nodiscard]] constexpr bool Suppress(UINT message, std::uint32_t key,
        std::uintptr_t details, HdrSettingsCaptureState capture) noexcept
    {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
            return SuppressDown(key, details, capture);
        if (message == WM_KEYUP || message == WM_SYSKEYUP)
            return SuppressUp(key);
        constexpr std::array characterMessages{WM_CHAR, WM_SYSCHAR, WM_UNICHAR};
        if (std::ranges::find(characterMessages, message) != characterMessages.end())
            return capture.active || CapturedScan(details);
        return false;
    }

    constexpr void Reset() noexcept { keys.fill(false); scans.fill(false); }

private:
    [[nodiscard]] constexpr bool SuppressDown(std::uint32_t key,
        std::uintptr_t details, HdrSettingsCaptureState capture) noexcept
    {
        if (key >= keys.size()) return false;
        if ((details & (1ULL << 30U)) == 0)
        {
            keys[key] = capture.capturedDown;
            const auto scan = (details >> 16U) & 0x1FFU;
            if (scan != 0) scans[scan] = capture.capturedDown;
        }
        return keys[key];
    }

    [[nodiscard]] constexpr bool SuppressUp(std::uint32_t key) noexcept
    {
        if (key >= keys.size()) return false;
        const auto suppress = keys[key];
        keys[key] = false;
        return suppress;
    }

    [[nodiscard]] constexpr bool CapturedScan(std::uintptr_t details) const noexcept
    {
        const auto scan = (details >> 16U) & 0x1FFU;
        return scan != 0 && scans[scan];
    }
};

// The window procedure handles hotkeys and HWND-affine settings interaction,
// preserves unowned key gestures, and queues bounded, trivially-copyable messages
// for ImGui to consume on the render thread.
[[nodiscard]] bool InitializeHdrSettingsInput(
    HWND window) noexcept;
void SetHdrSettingsHotkey(HdrSettingsHotkey hotkey) noexcept;
void BeginHdrSettingsHotkeyCapture() noexcept;
void CancelHdrSettingsHotkeyCapture() noexcept;
[[nodiscard]] bool HdrSettingsHotkeyCaptureActive() noexcept;
[[nodiscard]] bool ConsumeHdrSettingsHotkeyCapture(
    HdrSettingsHotkey& hotkey) noexcept;
void ShutdownHdrSettingsInput() noexcept;
void DrainHdrSettingsInput() noexcept;
[[nodiscard]] bool ConsumeHdrSettingsToggle() noexcept;
[[nodiscard]] bool ConsumeHdrSettingsFocusLoss() noexcept;
[[nodiscard]] bool HdrSettingsFocusLossPending() noexcept;
void SetHdrSettingsInputCapture(
    bool windowOpen, bool keyboard, bool mouse) noexcept;
[[nodiscard]] bool HdrSettingsInputInstalled() noexcept;
[[nodiscard]] std::uint64_t HdrSettingsInputOverflowCount() noexcept;

}  // namespace fo4hdr
