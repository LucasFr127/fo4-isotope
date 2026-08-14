// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/ui/HdrSettingsInput.hpp"

#include <algorithm>
#include <windowsx.h>

namespace fo4hdr {
namespace {

constexpr std::uint32_t keyboardCapture = 1;
constexpr std::uint32_t mouseCapture = 2;
constexpr UINT toggleMessage = WM_APP + 1;

bool KeyDown(UINT message) noexcept
{
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
}

bool KeyUp(UINT message) noexcept
{
    return message == WM_KEYUP || message == WM_SYSKEYUP;
}

bool KeyboardMessage(UINT message) noexcept
{
    return KeyDown(message) || KeyUp(message);
}

bool CharacterMessage(UINT message) noexcept
{
    constexpr std::array messages{WM_CHAR, WM_SYSCHAR, WM_UNICHAR};
    return std::ranges::find(messages, message) != messages.end();
}
std::uint32_t ReadModifiers() noexcept
{
    std::uint32_t modifiers{};
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= 1;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= 4;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= 8;
    return modifiers;
}

UINT PhysicalKey(WPARAM key, LPARAM details) noexcept
{
    const auto extended = (static_cast<std::uintptr_t>(details) & (1U << 24)) != 0;
    if (key == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (key == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    if (key == VK_SHIFT)
    {
        const auto scan = (static_cast<std::uintptr_t>(details) >> 16) & 0xFFU;
        return MapVirtualKeyW(static_cast<UINT>(scan), MAPVK_VSC_TO_VK_EX);
    }
    return static_cast<UINT>(key);
}

struct ButtonEvent
{
    UINT message;
    unsigned button;
    bool down;
};

constexpr std::array buttonEvents{
    ButtonEvent{WM_LBUTTONDOWN, 0, true}, ButtonEvent{WM_LBUTTONDBLCLK, 0, true},
    ButtonEvent{WM_LBUTTONUP, 0, false}, ButtonEvent{WM_RBUTTONDOWN, 1, true},
    ButtonEvent{WM_RBUTTONDBLCLK, 1, true}, ButtonEvent{WM_RBUTTONUP, 1, false},
    ButtonEvent{WM_MBUTTONDOWN, 2, true}, ButtonEvent{WM_MBUTTONDBLCLK, 2, true},
    ButtonEvent{WM_MBUTTONUP, 2, false}, ButtonEvent{WM_XBUTTONDOWN, 3, true},
    ButtonEvent{WM_XBUTTONDBLCLK, 3, true}, ButtonEvent{WM_XBUTTONUP, 3, false}};

void CopyMessage(const HdrSettingsInputMessage& record, std::span<HdrSettingsInputMessage> messages,
    HdrSettingsInputSnapshot& snapshot) noexcept
{
    if (record.message == toggleMessage)
    {
        ++snapshot.toggles;
        return;
    }
    if (snapshot.messages == messages.size())
    {
        snapshot.focusLost = true;
        return;
    }
    messages[snapshot.messages++] = record;
}
}  // namespace

bool HdrSettingsInput::Initialize(HWND window, DWORD consumerThread) noexcept
{
    if (!window || window_.load(std::memory_order_acquire)) return false;
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (thread != GetCurrentThreadId() || process != GetCurrentProcessId()) return false;
    read_.store(0, std::memory_order_relaxed);
    write_.store(0, std::memory_order_relaxed);
    resetRequested_.store(false, std::memory_order_relaxed);
    capture_.store(0, std::memory_order_relaxed);
    ResetProducer();
    focused_.store(GetFocus() == window, std::memory_order_relaxed);
    producerThread_.store(thread, std::memory_order_release);
    consumerThread_.store(consumerThread ? consumerThread : thread, std::memory_order_release);
    window_.store(window, std::memory_order_release);
    return true;
}

bool HdrSettingsInput::OnProducerThread() const noexcept
{
    return producerThread_.load(std::memory_order_acquire) == GetCurrentThreadId();
}

bool HdrSettingsInput::OnConsumerThread() const noexcept
{
    return consumerThread_.load(std::memory_order_acquire) == GetCurrentThreadId();
}

bool HdrSettingsInput::Matches(HWND window, DWORD consumerThread) const noexcept
{
    if (!window) return false;
    return window_.load(std::memory_order_acquire) == window &&
        consumerThread_.load(std::memory_order_acquire) == consumerThread;
}

void HdrSettingsInput::ReleaseOwnedCapture() noexcept
{
    if (!ownedCapture_) return;
    ownedCapture_ = false;
    if (GetCapture() != window_.load(std::memory_order_acquire)) return;
    releasingCapture_ = true;
    (void)ReleaseCapture();
    releasingCapture_ = false;
}

void HdrSettingsInput::ResetProducer() noexcept
{
    capturedKeys_.fill(false);
    capturedScans_.fill(false);
    capturedButtons_.fill(false);
    mouseButtons_ = 0;
    toggleArmed_ = false;
    ReleaseOwnedCapture();
}

bool HdrSettingsInput::Stop() noexcept
{
    if (!OnProducerThread()) return false;
    ResetProducer();
    window_.store(nullptr, std::memory_order_release);
    capture_.store(0, std::memory_order_release);
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    resetRequested_.store(false, std::memory_order_release);
    return true;
}

void HdrSettingsInput::RequestReset() noexcept
{
    // Preserve consumed down/up pairs even while dropping queued events.
    toggleArmed_ = false;
    ReleaseOwnedCapture();
    capture_.store(0, std::memory_order_release);
    resetRequested_.store(true, std::memory_order_release);
}

void HdrSettingsInput::Push(HdrSettingsInputMessage message) noexcept
{
    if (resetRequested_.load(std::memory_order_acquire)) return;
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % capacity;
    if (next == read_.load(std::memory_order_acquire))
    {
        RequestReset();
        return;
    }
    messages_[write] = message;
    write_.store(next, std::memory_order_release);
}

bool HdrSettingsInput::KeyGesture(UINT key, bool down, bool repeat, LPARAM details) noexcept
{
    if (key >= capturedKeys_.size()) return false;
    if (down && !repeat)
    {
        capturedKeys_[key] = key == VK_F10 ||
            (capture_.load(std::memory_order_acquire) & keyboardCapture) != 0;
        const auto scan = (static_cast<std::uintptr_t>(details) >> 16) & 0x1FFU;
        if (scan != 0) capturedScans_[scan] = key == VK_F10;
    }
    const auto consumed = capturedKeys_[key];
    if (!down) capturedKeys_[key] = false;
    return consumed;
}

bool HdrSettingsInput::ObserveKeyboard(UINT message, WPARAM key, LPARAM details) noexcept
{
    const auto physical = PhysicalKey(key, details);
    const auto down = KeyDown(message);
    const auto repeat = (static_cast<std::uintptr_t>(details) & (1U << 30)) != 0;
    const auto consumed = KeyGesture(physical, down, repeat, details);
    if (physical == VK_F10)
    {
        ToggleKey(down, repeat);
        return consumed;
    }
    Push({message, physical, details, ReadModifiers()});
    return consumed;
}

void HdrSettingsInput::ToggleKey(bool down, bool repeat) noexcept
{
    if (down)
    {
        if (!repeat && !resetRequested_.load(std::memory_order_acquire)) toggleArmed_ = true;
        return;
    }
    if (toggleArmed_) Push({toggleMessage});
    toggleArmed_ = false;
}
bool HdrSettingsInput::ObserveCharacter(UINT message, WPARAM key, LPARAM details) noexcept
{
    // UNICODE_NOCHAR is the native capability probe, not a Unicode scalar.
    if (message == WM_UNICHAR && key == UNICODE_NOCHAR) return true;
    const auto scan = (static_cast<std::uintptr_t>(details) >> 16) & 0x1FFU;
    if (!capturedScans_[scan]) Push({message, key, details, ReadModifiers()});
    return capturedScans_[scan] ||
        (capture_.load(std::memory_order_acquire) & keyboardCapture) != 0;
}

bool HdrSettingsInput::ObserveMouse(UINT message, WPARAM key, LPARAM details) noexcept
{
    const auto found = std::ranges::find(buttonEvents, message, &ButtonEvent::message);
    if (found == buttonEvents.end())
    {
        Push({message, key, details, ReadModifiers()});
        return (capture_.load(std::memory_order_acquire) & mouseCapture) != 0;
    }
    const auto button = found->button +
        (found->button == 3 && GET_XBUTTON_WPARAM(key) == XBUTTON2 ? 1U : 0U);
    const auto consumed = ButtonGesture(button, found->down);
    Push({message, key, details, ReadModifiers()});
    return consumed;
}

void HdrSettingsInput::BeginMouseGesture(unsigned button) noexcept
{
    capturedButtons_[button] = (capture_.load(std::memory_order_acquire) & mouseCapture) != 0;
    mouseButtons_ |= 1U << button;
    if (!capturedButtons_[button]) return;
    const auto window = window_.load(std::memory_order_acquire);
    if (GetCapture() == window) return;
    (void)SetCapture(window);
    ownedCapture_ = GetCapture() == window;
}

bool HdrSettingsInput::ButtonGesture(unsigned button, bool down) noexcept
{
    if (down)
    {
        BeginMouseGesture(button);
        return capturedButtons_[button];
    }
    const auto consumed = capturedButtons_[button];
    mouseButtons_ &= ~(1U << button);
    capturedButtons_[button] = false;
    if (mouseButtons_ == 0) ReleaseOwnedCapture();
    return consumed;
}

bool HdrSettingsInput::ResetMessage(UINT message) const noexcept
{
    if (message == WM_KILLFOCUS) return true;
    return message == WM_CAPTURECHANGED && !releasingCapture_;
}
bool HdrSettingsInput::Observe(HWND window, UINT message, WPARAM key, LPARAM details) noexcept
{
    if (!window || !OnProducerThread()) return false;
    if (window != window_.load(std::memory_order_acquire)) return false;
    if (message == WM_KILLFOCUS) focused_.store(false, std::memory_order_release);
    if (ResetMessage(message))
    {
        RequestReset();
        return false;
    }
    return ObserveMessage(message, key, details);
}

bool HdrSettingsInput::ObserveMessage(UINT message, WPARAM key, LPARAM details) noexcept
{
    if (KeyboardMessage(message)) return ObserveKeyboard(message, key, details);
    if (CharacterMessage(message))
        return ObserveCharacter(message, key, details);
    if (message == WM_SETFOCUS)
    {
        focused_.store(true, std::memory_order_release);
        Push({message});
    }
    const bool pointer = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    return pointer && ObserveMouse(message, key, details);
}

void HdrSettingsInput::SetCapture(bool open, bool keyboard, bool mouse) noexcept
{
    if (!OnConsumerThread()) return;
    std::uint32_t value{};
    if (open && keyboard) value |= keyboardCapture;
    if (open && mouse) value |= mouseCapture;
    capture_.store(value, std::memory_order_release);
}

HdrSettingsInputSnapshot HdrSettingsInput::Drain(std::span<HdrSettingsInputMessage> messages) noexcept
{
    HdrSettingsInputSnapshot snapshot;
    if (!OnConsumerThread() || !window_.load(std::memory_order_acquire)) return snapshot;
    snapshot.focused = focused_.load(std::memory_order_acquire);
    auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    if (resetRequested_.load(std::memory_order_acquire))
    {
        // The producer pauses writes until the consumer clears its cursor and
        // acknowledges; neither thread resets the other thread's native state.
        const auto pausedWrite = write_.load(std::memory_order_acquire);
        snapshot.focused = focused_.load(std::memory_order_acquire);
        read_.store(pausedWrite, std::memory_order_release);
        resetRequested_.store(false, std::memory_order_release);
        snapshot.focusLost = true;
        return snapshot;
    }
    while (read != write)
    {
        const auto& record = messages_[read];
        CopyMessage(record, messages, snapshot);
        read = (read + 1) % capacity;
    }
    read_.store(read, std::memory_order_release);
    if (snapshot.focusLost) snapshot.messages = snapshot.toggles = 0;
    return snapshot;
}

namespace {

enum class HostState { empty, preparing, requested, installing, installed, retired };

struct SettingsInputHost
{
    std::atomic<HostState> state{HostState::empty};
    std::atomic<WNDPROC> next{};
    std::atomic<bool> consumerReady{};
    HWND window{};
    DWORD windowThread{};
    DWORD consumerThread{};
    std::uint64_t generation{};
    HdrSettingsInput input;
};

// No shutdown unhook or queue reset: reachable callback/record code is pinned.
SettingsInputHost inputHost;

LRESULT CALLBACK SettingsHostWindowProcedure(HWND window, UINT message,
    WPARAM key, LPARAM details) noexcept;

bool PinProcedure(WNDPROC procedure) noexcept
{
    HMODULE module{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(procedure), &module) != 0 && module != nullptr;
}

bool NativeWindow(HWND window, DWORD& thread) noexcept
{
    if (!window || !IsWindow(window)) return false;
    DWORD process{};
    thread = GetWindowThreadProcessId(window, &process);
    return thread != 0 && process == GetCurrentProcessId();
}

bool HostMessage(HWND window, UINT message, WPARAM key, LPARAM details) noexcept
{
    if (inputHost.state.load(std::memory_order_acquire) != HostState::installed) return false;
    if (window != inputHost.window) return false;
    const auto consumed = inputHost.input.Observe(window, message, key, details);
    return consumed && inputHost.consumerReady.load(std::memory_order_acquire);
}

LRESULT ForwardHostMessage(HWND window, UINT message, WPARAM key, LPARAM details) noexcept
{
    const auto next = inputHost.next.load(std::memory_order_acquire);
    if (!next || next == SettingsHostWindowProcedure)
        return DefWindowProcW(window, message, key, details);
    try { return CallWindowProcW(next, window, message, key, details); }
    catch (...) { return 0; }
}

LRESULT CALLBACK SettingsHostWindowProcedure(HWND window, UINT message,
    WPARAM key, LPARAM details) noexcept
{
    const auto consumed = HostMessage(window, message, key, details);
    if (message == WM_NCDESTROY) RevokeSettingsInputHost();
    if (!consumed) return ForwardHostMessage(window, message, key, details);
    return message == WM_UNICHAR || message == WM_XBUTTONDOWN ||
        message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK ? TRUE : 0;
}

bool PrepareHostProcedure(WNDPROC& expected) noexcept
{
    expected = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(inputHost.window, GWLP_WNDPROC));
    if (!expected || expected == SettingsHostWindowProcedure) return false;
    if (!PinProcedure(expected) || !PinProcedure(SettingsHostWindowProcedure)) return false;
    return inputHost.input.Initialize(inputHost.window, inputHost.consumerThread);
}

bool InstallHostProcedure() noexcept
{
    WNDPROC expected{};
    if (!PrepareHostProcedure(expected)) return false;
    // Passive forwarding exists before the HWND-owner subclass exchange.
    inputHost.next.store(expected, std::memory_order_release);
    SetLastError(ERROR_SUCCESS);
    const auto old = SetWindowLongPtrW(inputHost.window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(SettingsHostWindowProcedure));
    if (old == 0 && GetLastError() != ERROR_SUCCESS) return false;
    const auto captured = reinterpret_cast<WNDPROC>(old);
    if (!captured || captured == SettingsHostWindowProcedure) return false;
    inputHost.next.store(captured, std::memory_order_release);
    // Direct subclass exchange is not compare-and-swap. Interference revokes
    // admission without overwriting the returned native forwarding chain.
    return captured == expected;
}

}  // namespace

bool PublishSettingsInputHost(HWND window, std::uint64_t generation) noexcept
{
    if (generation == 0) return false;
    DWORD thread{};
    if (!NativeWindow(window, thread)) return false;
    auto expected = HostState::empty;
    if (!inputHost.state.compare_exchange_strong(expected, HostState::preparing,
            std::memory_order_acq_rel)) return false;
    inputHost.window = window;
    inputHost.windowThread = thread;
    inputHost.consumerThread = GetCurrentThreadId();
    inputHost.generation = generation;
    expected = HostState::preparing;
    return inputHost.state.compare_exchange_strong(expected, HostState::requested,
        std::memory_order_release);
}

bool TryActivateSettingsInputHostOnCurrentThread() noexcept
{
    if (inputHost.state.load(std::memory_order_acquire) != HostState::requested) return false;
    DWORD thread{};
    if (!NativeWindow(inputHost.window, thread) || thread != GetCurrentThreadId()) return false;
    if (thread != inputHost.windowThread) return false;
    auto expected = HostState::requested;
    if (!inputHost.state.compare_exchange_strong(expected, HostState::installing,
            std::memory_order_acq_rel)) return false;
    if (!InstallHostProcedure())
    {
        RevokeSettingsInputHost();
        return false;
    }
    expected = HostState::installing;
    return inputHost.state.compare_exchange_strong(expected, HostState::installed,
        std::memory_order_release);
}

HdrSettingsInput* SettingsInputForGeneration(std::uint64_t generation) noexcept
{
    if (inputHost.state.load(std::memory_order_acquire) != HostState::installed) return nullptr;
    if (inputHost.generation != generation || inputHost.consumerThread != GetCurrentThreadId()) return nullptr;
    return &inputHost.input;
}

bool SetSettingsInputHostConsumerReady(std::uint64_t generation, bool ready) noexcept
{
    auto* input = SettingsInputForGeneration(generation);
    if (!input) return false;
    if (!ready) input->SetCapture(false, false, false);
    inputHost.consumerReady.store(ready, std::memory_order_release);
    return true;
}

void RevokeSettingsInputHost() noexcept
{
    if (inputHost.state.load(std::memory_order_acquire) == HostState::empty) return;
    inputHost.consumerReady.store(false, std::memory_order_release);
    inputHost.state.store(HostState::retired, std::memory_order_release);
}

}  // namespace fo4hdr
