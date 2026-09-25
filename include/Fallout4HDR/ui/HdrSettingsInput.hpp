// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fo4hdr {

struct HdrSettingsInputMessage
{
    UINT message{};
    WPARAM key{};
    LPARAM details{};
    std::uint32_t modifiers{};
};

struct HdrSettingsInputSnapshot
{
    std::size_t messages{};
    std::uint32_t toggles{};
    bool focusLost{};
    bool focused{};
};

// One HWND-affine producer and one fixed consumer; callbacks only publish plain
// messages. The caller forwards unconsumed messages once and owns installation.
class HdrSettingsInput
{
public:
    // Both threads must be quiescent during cold Initialize/Stop. Initialize and
    // Stop run on the HWND owner; consumerThread defaults to that same thread.
    [[nodiscard]] bool Initialize(HWND window, DWORD consumerThread = 0) noexcept;
    [[nodiscard]] bool Stop() noexcept;
    [[nodiscard]] bool Observe(HWND window, UINT message, WPARAM key, LPARAM details) noexcept;
    [[nodiscard]] HdrSettingsInputSnapshot Drain(
        std::span<HdrSettingsInputMessage> messages = {}) noexcept;
    void SetCapture(bool open, bool keyboard, bool mouse) noexcept;
    [[nodiscard]] bool Matches(HWND window, DWORD consumerThread) const noexcept;
    // Same producer/consumer thread only; held gestures and reset handshakes refuse.
    [[nodiscard]] bool CanChangeConsumerReadiness() const noexcept;

    static constexpr std::size_t capacity = 256;

private:
    [[nodiscard]] bool OnProducerThread() const noexcept;
    [[nodiscard]] bool OnConsumerThread() const noexcept;
    [[nodiscard]] bool HasCapturedKeyboardGesture() const noexcept;
    [[nodiscard]] bool HasCapturedPointerGesture() const noexcept;
    void ResetProducer() noexcept;
    void RequestReset() noexcept;
    void Push(HdrSettingsInputMessage message) noexcept;
    [[nodiscard]] bool ObserveKeyboard(UINT message, WPARAM key, LPARAM details) noexcept;
    [[nodiscard]] bool ObserveMouse(UINT message, WPARAM key, LPARAM details) noexcept;
    [[nodiscard]] bool ObserveCharacter(UINT message, WPARAM key, LPARAM details) noexcept;
    [[nodiscard]] bool KeyGesture(UINT key, bool down, bool repeat, LPARAM details) noexcept;
    void ReleaseOwnedCapture() noexcept;
    void ToggleKey(bool down, bool repeat) noexcept;
    [[nodiscard]] bool ButtonGesture(unsigned button, bool down) noexcept;
    void BeginMouseGesture(unsigned button) noexcept;
    [[nodiscard]] bool ResetMessage(UINT message) const noexcept;
    [[nodiscard]] bool ObserveMessage(UINT message, WPARAM key, LPARAM details) noexcept;

    std::array<HdrSettingsInputMessage, capacity> messages_{};
    std::array<bool, 256> capturedKeys_{};
    std::array<bool, 512> capturedScans_{};
    std::array<bool, 5> capturedButtons_{};
    std::atomic<std::size_t> write_{};
    std::atomic<std::size_t> read_{};
    std::atomic<bool> resetRequested_{};
    std::atomic<HWND> window_{};
    std::atomic<DWORD> producerThread_{};
    std::atomic<DWORD> consumerThread_{};
    std::atomic<std::uint32_t> capture_{};
    std::atomic<bool> focused_{};
    std::uint32_t mouseButtons_{};
    bool ownedCapture_{};
    bool releasingCapture_{};
    bool toggleArmed_{};
};

// One process-resident HWND/generation. Publish on the cold renderer owner;
// activation verifies and runs on the HWND owner. Retirement is passive and
// never resets a queue from another thread or restores a foreign subclass.
// Activation requires cooperative exclusive subclass installation: no foreign
// concurrent WNDPROC mutation. A mismatched exchange revokes admission; keeping
// its captured forwarding chain resident remains the interfering owner's duty.
[[nodiscard]] bool PublishSettingsInputHost(HWND window, std::uint64_t generation) noexcept;
[[nodiscard]] bool TryActivateSettingsInputHostOnCurrentThread() noexcept;
[[nodiscard]] HdrSettingsInput* SettingsInputForGeneration(std::uint64_t generation) noexcept;
// A real consumer opts in only after readiness; every readiness transition and
// revocation requires input/render callbacks and captured gestures quiescent.
// Without opt-in every native message, including F10, continues through the
// captured predecessor.
[[nodiscard]] bool SetSettingsInputHostConsumerReady(std::uint64_t generation, bool ready) noexcept;
void RevokeSettingsInputHost() noexcept;

}  // namespace fo4hdr
