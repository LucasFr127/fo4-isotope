// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/ui/HdrSettingsInput.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {
namespace {

struct InputMessage
{
    UINT message{};
    WPARAM wParam{};
    LPARAM lParam{};
};

constexpr std::size_t kInputCapacity = 256;
constexpr std::size_t kMaximumDrainPerFrame = kInputCapacity;

std::array<InputMessage, kInputCapacity> g_messages{};
std::atomic<std::size_t> g_writeIndex{};
std::atomic<std::size_t> g_readIndex{};
std::atomic<std::uint64_t> g_overflowCount{};
std::atomic<std::uint32_t> g_toggleCount{};
std::atomic<bool> g_focusLoss{};
std::atomic<std::uint32_t> g_toggleHotkey{
    kDefaultSettingsToggleVirtualKey | (kSettingsHotkeyAlt << 8U)};
std::atomic<bool> g_hotkeyCapture{};
std::atomic<std::uint32_t> g_capturedHotkey{};
std::atomic<std::uint32_t> g_shortcutReset{};
// Only the owning window thread accesses these event-sequence fields.
HdrSettingsShortcutLatch g_shortcutLatch{};
HdrSettingsCapturedGestureFilter g_capturedGestureFilter{};
HdrSettingsHotkey g_capturePending{};
bool g_capturePendingValid{};
std::uint32_t g_seenShortcutReset{};
std::array<bool, 256> g_swallowedKeys{};
std::atomic<bool> g_resetRequested{};
std::atomic<bool> g_windowOpen{};
std::atomic<bool> g_captureKeyboard{};
std::atomic<bool> g_captureMouse{};
std::atomic<bool> g_active{};
std::atomic<bool> g_installed{};
std::atomic<bool> g_retainedForeignChain{};
std::atomic<HWND> g_window{};
std::atomic<DWORD> g_windowThread{};
std::atomic<WNDPROC> g_nextWindowProcedure{};
std::atomic<UINT> g_interactionMessage{};
std::atomic<HdrSettingsInteractionRequestHandler>
    g_interactionRequestHandler{};
std::atomic<std::uint64_t> g_interactionStatus{
    static_cast<std::uint8_t>(HdrSettingsInteractionState::unavailable)};

constexpr std::uint64_t kInteractionStateMask = 0xFFU;
constexpr wchar_t kInteractionMessageName[] =
    L"FO4Isotope.SettingsInteraction.v1";

void ResetQueue() noexcept;

[[nodiscard]] constexpr std::uint32_t PackHotkey(HdrSettingsHotkey key) noexcept
{
    return key.virtualKey | (key.modifiers << 8U);
}

[[nodiscard]] constexpr HdrSettingsHotkey UnpackHotkey(std::uint32_t key) noexcept
{
    return {key & 0xFFU, key >> 8U};
}

[[nodiscard]] std::uint32_t CurrentModifiers() noexcept
{
    return ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ? 1U : 0U) |
        ((GetKeyState(VK_MENU) & 0x8000) != 0 ? 2U : 0U) |
        ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 4U : 0U) |
        (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0 ? 8U : 0U);
}

[[nodiscard]] constexpr std::uint64_t PackInteractionStatus(
    std::uint32_t generation,
    HdrSettingsInteractionState state) noexcept
{
    return (static_cast<std::uint64_t>(generation) << 8U) |
        static_cast<std::uint8_t>(state);
}

[[nodiscard]] constexpr std::uint32_t InteractionGeneration(
    std::uint64_t status) noexcept
{
    return static_cast<std::uint32_t>(status >> 8U);
}

[[nodiscard]] constexpr HdrSettingsInteractionState InteractionState(
    std::uint64_t status) noexcept
{
    return static_cast<HdrSettingsInteractionState>(
        status & kInteractionStateMask);
}

[[nodiscard]] std::uint32_t BeginInteractionRequest(
    HdrSettingsInteractionState state) noexcept
{
    auto current = g_interactionStatus.load(std::memory_order_acquire);
    for (;;)
    {
        auto generation = InteractionGeneration(current) + 1U;
        if (generation == 0) generation = 1;
        const auto requested = PackInteractionStatus(generation, state);
        if (g_interactionStatus.compare_exchange_weak(
                current,
                requested,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            return generation;
    }
}

void CompleteInteractionRequest(
    std::uint32_t generation,
    HdrSettingsInteractionState state) noexcept
{
    auto current = g_interactionStatus.load(std::memory_order_acquire);
    while (InteractionGeneration(current) == generation)
    {
        const auto completed = PackInteractionStatus(generation, state);
        if (g_interactionStatus.compare_exchange_weak(
                current,
                completed,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            return;
    }
}

[[nodiscard]] bool ApplyInteractionRequest(
    std::uint32_t generation, bool open) noexcept
{
    const auto current =
        g_interactionStatus.load(std::memory_order_acquire);
    const auto expectedState = open ?
        HdrSettingsInteractionState::opening :
        HdrSettingsInteractionState::closing;
    if (InteractionGeneration(current) != generation ||
        InteractionState(current) != expectedState)
        return true;

    const auto handler =
        g_interactionRequestHandler.load(std::memory_order_acquire);
    if (handler == nullptr)
    {
        CompleteInteractionRequest(
            generation, HdrSettingsInteractionState::unavailable);
        return false;
    }

    try
    {
        const auto accepted = handler(open);
        CompleteInteractionRequest(
            generation,
            accepted ? (open ? HdrSettingsInteractionState::active :
                               HdrSettingsInteractionState::inactive) :
                       HdrSettingsInteractionState::failed);
        return accepted;
    }
    catch (...)
    {
        CompleteInteractionRequest(
            generation, HdrSettingsInteractionState::failed);
        return false;
    }
}

[[nodiscard]] constexpr bool IsKeyboardMessage(UINT message) noexcept
{
    constexpr std::array messages{WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_UNICHAR};
    return std::ranges::find(messages, message) != messages.end();
}

[[nodiscard]] ImGuiKey ToImGuiKey(WPARAM key) noexcept
{
    if (key >= '0' && key <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (key - '0'));
    if (key >= 'A' && key <= 'Z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - 'A'));
    if (key >= VK_F1 && key <= VK_F12)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - VK_F1));
    struct KeyMapping { WPARAM native; ImGuiKey imgui; };
    constexpr KeyMapping mappings[]{
        {VK_TAB, ImGuiKey_Tab},
        {VK_LEFT, ImGuiKey_LeftArrow},
        {VK_RIGHT, ImGuiKey_RightArrow},
        {VK_UP, ImGuiKey_UpArrow},
        {VK_DOWN, ImGuiKey_DownArrow},
        {VK_PRIOR, ImGuiKey_PageUp},
        {VK_NEXT, ImGuiKey_PageDown},
        {VK_HOME, ImGuiKey_Home},
        {VK_END, ImGuiKey_End},
        {VK_INSERT, ImGuiKey_Insert},
        {VK_DELETE, ImGuiKey_Delete},
        {VK_BACK, ImGuiKey_Backspace},
        {VK_SPACE, ImGuiKey_Space},
        {VK_RETURN, ImGuiKey_Enter},
        {VK_ESCAPE, ImGuiKey_Escape},
        {VK_OEM_7, ImGuiKey_Apostrophe},
        {VK_OEM_COMMA, ImGuiKey_Comma},
        {VK_OEM_MINUS, ImGuiKey_Minus},
        {VK_OEM_PERIOD, ImGuiKey_Period},
        {VK_OEM_2, ImGuiKey_Slash},
        {VK_OEM_1, ImGuiKey_Semicolon},
        {VK_OEM_PLUS, ImGuiKey_Equal},
        {VK_OEM_4, ImGuiKey_LeftBracket},
        {VK_OEM_5, ImGuiKey_Backslash},
        {VK_OEM_6, ImGuiKey_RightBracket},
        {VK_OEM_3, ImGuiKey_GraveAccent},
        {VK_CAPITAL, ImGuiKey_CapsLock},
        {VK_SCROLL, ImGuiKey_ScrollLock},
        {VK_NUMLOCK, ImGuiKey_NumLock},
        {VK_SNAPSHOT, ImGuiKey_PrintScreen},
        {VK_PAUSE, ImGuiKey_Pause},
        {VK_NUMPAD0, ImGuiKey_Keypad0},
        {VK_NUMPAD1, ImGuiKey_Keypad1},
        {VK_NUMPAD2, ImGuiKey_Keypad2},
        {VK_NUMPAD3, ImGuiKey_Keypad3},
        {VK_NUMPAD4, ImGuiKey_Keypad4},
        {VK_NUMPAD5, ImGuiKey_Keypad5},
        {VK_NUMPAD6, ImGuiKey_Keypad6},
        {VK_NUMPAD7, ImGuiKey_Keypad7},
        {VK_NUMPAD8, ImGuiKey_Keypad8},
        {VK_NUMPAD9, ImGuiKey_Keypad9},
        {VK_DECIMAL, ImGuiKey_KeypadDecimal},
        {VK_DIVIDE, ImGuiKey_KeypadDivide},
        {VK_MULTIPLY, ImGuiKey_KeypadMultiply},
        {VK_SUBTRACT, ImGuiKey_KeypadSubtract},
        {VK_ADD, ImGuiKey_KeypadAdd},
        {VK_LSHIFT, ImGuiKey_LeftShift},
        {VK_LCONTROL, ImGuiKey_LeftCtrl},
        {VK_LMENU, ImGuiKey_LeftAlt},
        {VK_LWIN, ImGuiKey_LeftSuper},
        {VK_RSHIFT, ImGuiKey_RightShift},
        {VK_RCONTROL, ImGuiKey_RightCtrl},
        {VK_RMENU, ImGuiKey_RightAlt},
        {VK_RWIN, ImGuiKey_RightSuper},
        {VK_APPS, ImGuiKey_Menu},
    };
    const auto found = std::ranges::find(mappings, key, &KeyMapping::native);
    return found == std::end(mappings) ? ImGuiKey_None : found->imgui;
}

void AddModifierEvents(ImGuiIO& io) noexcept
{
    io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Super,
        (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetKeyState(VK_RWIN) & 0x8000) != 0);
}

bool IsKeyDown(UINT message) noexcept
{
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
}

bool IsKeyUp(UINT message) noexcept
{
    return message == WM_KEYUP || message == WM_SYSKEYUP;
}

void AddKeyboardEvent(ImGuiIO& io, const InputMessage& record) noexcept
{
    AddModifierEvents(io);
    const auto key = ToImGuiKey(record.wParam);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, IsKeyDown(record.message));
}

void AddCharacterEvent(ImGuiIO& io, const InputMessage& record) noexcept
{
    if (record.wParam > 0 && record.wParam <= 0xFFFFU)
        io.AddInputCharacterUTF16(static_cast<ImWchar16>(record.wParam));
}

void AddMouseButtonEvent(ImGuiIO& io, const InputMessage& record) noexcept
{
    struct ButtonMessage { UINT message; int button; bool down; };
    constexpr ButtonMessage messages[]{
        {WM_LBUTTONDOWN, 0, true}, {WM_LBUTTONDBLCLK, 0, true}, {WM_LBUTTONUP, 0, false},
        {WM_RBUTTONDOWN, 1, true}, {WM_RBUTTONDBLCLK, 1, true}, {WM_RBUTTONUP, 1, false},
        {WM_MBUTTONDOWN, 2, true}, {WM_MBUTTONDBLCLK, 2, true}, {WM_MBUTTONUP, 2, false},
        {WM_XBUTTONDOWN, 3, true}, {WM_XBUTTONDBLCLK, 3, true}, {WM_XBUTTONUP, 3, false}};
    const auto found = std::ranges::find(messages, record.message, &ButtonMessage::message);
    if (found == std::end(messages)) return;
    auto button = found->button;
    if (button == 3 && GET_XBUTTON_WPARAM(record.wParam) != XBUTTON1) button = 4;
    io.AddMouseButtonEvent(button, found->down);
}

void AddPointerEvent(ImGuiIO& io, const InputMessage& record) noexcept
{
    if (record.message == WM_MOUSEMOVE)
        io.AddMousePosEvent(
            static_cast<float>(static_cast<short>(LOWORD(record.lParam))),
            static_cast<float>(static_cast<short>(HIWORD(record.lParam))));
    else if (record.message == WM_MOUSEWHEEL)
        io.AddMouseWheelEvent(0.0F,
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(record.wParam)) / static_cast<float>(WHEEL_DELTA));
    else if (record.message == WM_MOUSEHWHEEL)
        io.AddMouseWheelEvent(
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(record.wParam)) / static_cast<float>(WHEEL_DELTA), 0.0F);
    else AddMouseButtonEvent(io, record);
}

void AddImGuiEvent(const InputMessage& record) noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    if (IsKeyDown(record.message) || IsKeyUp(record.message)) AddKeyboardEvent(io, record);
    else if (record.message == WM_CHAR || record.message == WM_UNICHAR) AddCharacterEvent(io, record);
    else if (record.message == WM_SETFOCUS || record.message == WM_KILLFOCUS)
        io.AddFocusEvent(record.message == WM_SETFOCUS);
    else AddPointerEvent(io, record);
}
[[nodiscard]] constexpr bool IsMouseMessage(UINT message) noexcept
{
    constexpr std::array messages{WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_MOUSEHWHEEL, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK, WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK};
    return std::ranges::find(messages, message) != messages.end();
}

[[nodiscard]] bool Enqueue(
    UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    const auto write = g_writeIndex.load(std::memory_order_relaxed);
    const auto next = (write + 1U) % kInputCapacity;
    if (next == g_readIndex.load(std::memory_order_acquire))
    {
        g_overflowCount.fetch_add(1, std::memory_order_relaxed);
        g_resetRequested.store(true, std::memory_order_release);
        return false;
    }
    g_messages[write] = {message, wParam, lParam};
    g_writeIndex.store(next, std::memory_order_release);
    return true;
}

[[nodiscard]] LRESULT CALLBACK HdrSettingsWindowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

[[nodiscard]] bool HasPredecessor(WNDPROC next) noexcept
{
    return next != nullptr && next != HdrSettingsWindowProcedure;
}

[[nodiscard]] LRESULT ForwardInput(
    WNDPROC next, HWND window, const InputMessage& input)
{
    if (HasPredecessor(next))
        return CallWindowProcW(next, window, input.message, input.wParam, input.lParam);
    return DefWindowProcW(window, input.message, input.wParam, input.lParam);
}

void CloseInteractionIfPending() noexcept
{
    const auto interaction = InteractionState(
        g_interactionStatus.load(std::memory_order_acquire));
    constexpr std::array pending{HdrSettingsInteractionState::opening,
        HdrSettingsInteractionState::active, HdrSettingsInteractionState::closing};
    if (std::ranges::find(pending, interaction) != pending.end())
        (void)RequestHdrSettingsInteraction(false);
}

void ClearWindowRecord() noexcept
{
    g_window.store(nullptr, std::memory_order_release);
    g_windowThread.store(0, std::memory_order_release);
    g_nextWindowProcedure.store(nullptr, std::memory_order_release);
}

void RetireDestroyedWindow(bool active) noexcept
{
    g_installed.store(false, std::memory_order_release);
    if (active) g_active.store(false, std::memory_order_release);
    g_retainedForeignChain.store(false, std::memory_order_release);
    ClearWindowRecord();
    ResetQueue();
}

void ApplyShortcutReset() noexcept
{
    const auto reset = g_shortcutReset.load(std::memory_order_acquire);
    if (reset == g_seenShortcutReset) return;
    g_seenShortcutReset = reset;
    g_shortcutLatch.Reset();
    g_capturePendingValid = false;
}

struct KeyGesture
{
    std::uint32_t key{};
    bool down{};
    bool up{};
    bool repeat{};

    [[nodiscard]] bool IsKeyboardEvent() const noexcept { return down || up; }
};

[[nodiscard]] bool CaptureKeyDown(std::uint32_t key) noexcept
{
    if (key == VK_ESCAPE)
    {
        CancelHdrSettingsHotkeyCapture();
        g_capturePendingValid = false;
        return true;
    }
    const HdrSettingsHotkey candidate{key, CurrentModifiers()};
    if (!IsSupportedHdrSettingsHotkey(candidate)) return false;
    g_capturePending = candidate;
    g_capturePendingValid = true;
    return true;
}

[[nodiscard]] bool CaptureKeyUp(std::uint32_t key) noexcept
{
    if (!g_capturePendingValid || key != g_capturePending.virtualKey) return false;
    g_capturedHotkey.store(PackHotkey(g_capturePending), std::memory_order_release);
    g_hotkeyCapture.store(false, std::memory_order_release);
    g_capturePendingValid = false;
    return true;
}

[[nodiscard]] bool CaptureShortcut(const KeyGesture& key) noexcept
{
    // Release completes capture so the selected shortcut cannot close the menu
    // during the gesture that configured it.
    g_shortcutLatch.Reset();
    bool consumed{};
    if (key.down && !key.repeat) consumed = CaptureKeyDown(key.key);
    if (key.up) consumed = CaptureKeyUp(key.key) || consumed;
    return consumed;
}

[[nodiscard]] bool ProcessShortcut(const KeyGesture& key, bool windowOpen) noexcept
{
    bool consumed{};
    if (key.down)
        consumed = g_shortcutLatch.KeyDown(
            UnpackHotkey(g_toggleHotkey.load(std::memory_order_acquire)),
            key.key, CurrentModifiers(), key.repeat);
    const auto toggle = key.up && g_shortcutLatch.KeyUp(key.key);
    if (toggle && RequestHdrSettingsInteraction(!windowOpen))
        g_toggleCount.fetch_add(1, std::memory_order_release);
    return consumed;
}

[[nodiscard]] bool IsUiInput(UINT message) noexcept
{
    return IsKeyboardMessage(message) || IsMouseMessage(message) ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS;
}

void LoseInputFocus(bool windowOpen) noexcept
{
    if (windowOpen) (void)RequestHdrSettingsInteraction(false);
    g_resetRequested.store(true, std::memory_order_release);
    g_windowOpen.store(false, std::memory_order_release);
    g_focusLoss.store(true, std::memory_order_release);
    g_shortcutLatch.Reset();
    g_capturePendingValid = false;
    g_swallowedKeys.fill(false);
    g_capturedGestureFilter.Reset();
    CancelHdrSettingsHotkeyCapture();
}

[[nodiscard]] bool CaptureKeyboardMessage(UINT message, bool windowOpen) noexcept
{
    // Generic keyboard capture leaves system chords in the game/OS chain.
    if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) return false;
    return IsKeyboardMessage(message) && windowOpen &&
        g_captureKeyboard.load(std::memory_order_acquire);
}

[[nodiscard]] bool SwallowKeyGesture(const KeyGesture& key, bool consumed) noexcept
{
    // A consumed down owns repeats and its matching up across menu/rebind changes.
    // A forwarded down must retain its release in the game/OS chain.
    if (key.down && !key.repeat) g_swallowedKeys[key.key] = consumed;
    const auto swallowed = g_swallowedKeys[key.key];
    if (key.up) g_swallowedKeys[key.key] = false;
    return swallowed;
}

struct GestureConsumption
{
    bool shortcut{};
    bool captured{};
};

[[nodiscard]] bool ConsumeGameInput(UINT message, bool windowOpen,
    const KeyGesture& key, GestureConsumption consumed) noexcept
{
    const auto consumeKeyboard = CaptureKeyboardMessage(message, windowOpen);
    const auto consumeMouse = IsMouseMessage(message) && windowOpen &&
        g_captureMouse.load(std::memory_order_acquire);
    const auto keyEvent = key.IsKeyboardEvent();
    if (keyEvent && key.key < g_swallowedKeys.size())
        return SwallowKeyGesture(key, consumed.shortcut || consumeKeyboard);
    return consumed.captured || consumeKeyboard || consumeMouse;
}

[[nodiscard]] KeyGesture ReadKeyGesture(const InputMessage& input) noexcept
{
    return {static_cast<std::uint32_t>(input.wParam),
        IsKeyDown(input.message), IsKeyUp(input.message),
        (static_cast<std::uintptr_t>(input.lParam) & (1ULL << 30U)) != 0};
}
[[nodiscard]] bool ConsumeInputMessage(const InputMessage& input) noexcept
{
    const auto windowOpen = g_windowOpen.load(std::memory_order_acquire);
    ApplyShortcutReset();
    const auto key = ReadKeyGesture(input);
    const auto capture = windowOpen && g_hotkeyCapture.load(std::memory_order_acquire);
    const auto shortcutConsumed = capture ? CaptureShortcut(key) : ProcessShortcut(key, windowOpen);
    const auto uiInput = windowOpen && IsUiInput(input.message);
    const auto capturedGesture = g_capturedGestureFilter.Suppress(input.message,
        key.key, static_cast<std::uintptr_t>(input.lParam), {key.down && shortcutConsumed, capture});
    if (uiInput && !capturedGesture) (void)Enqueue(input.message, input.wParam, input.lParam);
    if (input.message == WM_KILLFOCUS) LoseInputFocus(windowOpen);
    return ConsumeGameInput(input.message, windowOpen, key, {shortcutConsumed, capturedGesture});
}

[[nodiscard]] LRESULT ProcessActiveWindowMessage(
    WNDPROC next, HWND window, const InputMessage& input)
{
    if (ConsumeInputMessage(input)) return 0;
    if (!HasPredecessor(next))
        return DefWindowProcW(window, input.message, input.wParam, input.lParam);
    const auto result = CallWindowProcW(next, window, input.message, input.wParam, input.lParam);
    if (input.message == WM_NCDESTROY) RetireDestroyedWindow(true);
    return result;
}

[[nodiscard]] LRESULT ProcessWindowMessage(HWND window, const InputMessage& input)
{
    const auto next = g_nextWindowProcedure.load(std::memory_order_acquire);
    const auto interactionMessage = g_interactionMessage.load(std::memory_order_acquire);
    if (interactionMessage != 0 && input.message == interactionMessage)
    {
        (void)ApplyInteractionRequest(static_cast<std::uint32_t>(input.wParam), input.lParam != 0);
        return 0;
    }
    if (input.message == WM_NCDESTROY) CloseInteractionIfPending();
    if (!g_active.load(std::memory_order_acquire))
    {
        const auto result = ForwardInput(next, window, input);
        if (input.message == WM_NCDESTROY) RetireDestroyedWindow(false);
        return result;
    }
    return ProcessActiveWindowMessage(next, window, input);
}

[[nodiscard]] LRESULT CALLBACK HdrSettingsWindowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    const InputMessage input{message, wParam, lParam};
    try { return ProcessWindowMessage(window, input); }
    catch (...)
    {
        return ForwardInput(g_nextWindowProcedure.load(std::memory_order_acquire), window, input);
    }
}


void ResetQueue() noexcept
{
    const auto write = g_writeIndex.load(std::memory_order_acquire);
    g_readIndex.store(write, std::memory_order_release);
}

[[nodiscard]] bool CanInitializeInput(HWND window) noexcept
{
    if (window == nullptr || !IsWindow(window)) return false;
    if (!IsSupportedHdrSettingsHotkey(UnpackHotkey(
            g_toggleHotkey.load(std::memory_order_acquire)))) return false;
    return !g_retainedForeignChain.load(std::memory_order_acquire);
}

[[nodiscard]] bool ReactivateInput(HWND window) noexcept
{
    if (g_window.load(std::memory_order_acquire) != window) return false;
    if (g_retainedForeignChain.load(std::memory_order_acquire)) return false;
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) !=
            HdrSettingsWindowProcedure) return false;
    g_active.store(true, std::memory_order_release);
    return true;
}

[[nodiscard]] bool InstallInputPredecessor(HWND window) noexcept
{
    SetLastError(ERROR_SUCCESS);
    const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(HdrSettingsWindowProcedure));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS)
    {
        ClearWindowRecord();
        return false;
    }
    const auto next = reinterpret_cast<WNDPROC>(previous);
    if (!HasPredecessor(next))
    {
        ClearWindowRecord();
        return false;
    }
    // The returned predecessor is authoritative even if a same-thread owner
    // changed it before installation.
    g_nextWindowProcedure.store(next, std::memory_order_release);
    return true;
}

void ActivateInput() noexcept
{
    CancelHdrSettingsHotkeyCapture();
    ResetQueue();
    g_toggleCount.store(0, std::memory_order_release);
    g_resetRequested.store(false, std::memory_order_release);
    g_focusLoss.store(false, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
}

bool InitializeWindowInput(HWND window) noexcept
{
    DWORD process{};
    const auto windowThread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || windowThread != GetCurrentThreadId())
        return false;
    const auto interactionMessage = RegisterWindowMessageW(kInteractionMessageName);
    if (interactionMessage == 0) return false;
    const auto expected = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (!HasPredecessor(expected)) return false;
    // Publish passive forwarding before making the hook live. The owning
    // window thread cannot dispatch queued messages between these operations.
    g_window.store(window, std::memory_order_release);
    g_windowThread.store(windowThread, std::memory_order_release);
    g_interactionMessage.store(interactionMessage, std::memory_order_release);
    g_nextWindowProcedure.store(expected, std::memory_order_release);
    if (!InstallInputPredecessor(window)) return false;
    ActivateInput();
    return true;
}

struct InteractionDestination
{
    HdrSettingsInteractionRequestHandler handler{};
    HWND window{};
    DWORD thread{};
    UINT message{};

    [[nodiscard]] bool Valid() const noexcept
    {
        return handler != nullptr && window != nullptr && thread != 0 && message != 0;
    }
};

[[nodiscard]] bool PostInteractionRequest(const InteractionDestination& destination,
    std::uint32_t generation, bool open) noexcept
{
    if (PostMessageW(destination.window, destination.message,
            static_cast<WPARAM>(generation), open ? 1 : 0)) return true;
    CompleteInteractionRequest(generation, HdrSettingsInteractionState::failed);
    return false;
}

}  // namespace

bool InitializeHdrSettingsInput(HWND window) noexcept
{
    if (!CanInitializeInput(window)) return false;
    if (g_installed.load(std::memory_order_acquire)) return ReactivateInput(window);
    return InitializeWindowInput(window);
}
void ShutdownHdrSettingsInput() noexcept
{
    if (!g_installed.load(std::memory_order_acquire)) return;
    CloseInteractionIfPending();
    g_active.store(false, std::memory_order_release);
    const auto window = g_window.load(std::memory_order_acquire);
    const auto current = window != nullptr ?
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) :
        nullptr;
    g_windowOpen.store(false, std::memory_order_release);
    g_captureKeyboard.store(false, std::memory_order_release);
    g_captureMouse.store(false, std::memory_order_release);
    CancelHdrSettingsHotkeyCapture();
    g_shortcutReset.fetch_add(1, std::memory_order_release);
    // Keep the process-lifetime subclass passive instead of performing a
    // check-then-restore that could overwrite a concurrently installed owner.
    // It can be reactivated when still current; a later owner keeps the exact
    // passive predecessor chain valid through WM_NCDESTROY.
    g_retainedForeignChain.store(
        current != nullptr && current != HdrSettingsWindowProcedure,
        std::memory_order_release);
    ResetQueue();
}

void DrainHdrSettingsInput() noexcept
{
    if (g_resetRequested.exchange(false, std::memory_order_acq_rel))
    {
        ResetQueue();
        ImGuiIO& io = ImGui::GetIO();
        io.AddFocusEvent(false);
        return;
    }

    std::size_t drained{};
    auto read = g_readIndex.load(std::memory_order_relaxed);
    const auto write = g_writeIndex.load(std::memory_order_acquire);
    while (read != write && drained < kMaximumDrainPerFrame)
    {
        const auto record = g_messages[read];
        AddImGuiEvent(record);
        read = (read + 1U) % kInputCapacity;
        ++drained;
    }
    g_readIndex.store(read, std::memory_order_release);
}

bool ConsumeHdrSettingsToggle() noexcept
{
    auto count = g_toggleCount.load(std::memory_order_acquire);
    while (count != 0)
    {
        if (g_toggleCount.compare_exchange_weak(count, count - 1U,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }
    return false;
}

bool ConsumeHdrSettingsFocusLoss() noexcept
{
    return g_focusLoss.exchange(false, std::memory_order_acq_rel);
}

bool HdrSettingsFocusLossPending() noexcept
{
    return g_focusLoss.load(std::memory_order_acquire);
}

void SetHdrSettingsHotkey(HdrSettingsHotkey hotkey) noexcept
{
    if (!IsSupportedHdrSettingsHotkey(hotkey)) return;
    const auto packed = PackHotkey(hotkey);
    if (g_toggleHotkey.exchange(packed, std::memory_order_acq_rel) != packed)
        g_shortcutReset.fetch_add(1, std::memory_order_release);
}

void BeginHdrSettingsHotkeyCapture() noexcept
{
    g_capturedHotkey.store(0, std::memory_order_release);
    g_shortcutReset.fetch_add(1, std::memory_order_release);
    g_hotkeyCapture.store(true, std::memory_order_release);
}

void CancelHdrSettingsHotkeyCapture() noexcept
{
    g_hotkeyCapture.store(false, std::memory_order_release);
    g_capturedHotkey.store(0, std::memory_order_release);
    g_shortcutReset.fetch_add(1, std::memory_order_release);
}

bool HdrSettingsHotkeyCaptureActive() noexcept
{
    return g_hotkeyCapture.load(std::memory_order_acquire);
}

bool ConsumeHdrSettingsHotkeyCapture(HdrSettingsHotkey& hotkey) noexcept
{
    const auto packed = g_capturedHotkey.exchange(0, std::memory_order_acq_rel);
    if (packed == 0) return false;
    hotkey = UnpackHotkey(packed);
    return IsSupportedHdrSettingsHotkey(hotkey);
}

void SetHdrSettingsInputCapture(
    bool windowOpen, bool keyboard, bool mouse) noexcept
{
    g_windowOpen.store(windowOpen, std::memory_order_release);
    g_captureKeyboard.store(windowOpen && keyboard, std::memory_order_release);
    g_captureMouse.store(windowOpen && mouse, std::memory_order_release);
    if (!windowOpen && g_hotkeyCapture.load(std::memory_order_acquire))
        CancelHdrSettingsHotkeyCapture();
}

void RegisterHdrSettingsInteractionRequestHandler(
    HdrSettingsInteractionRequestHandler handler) noexcept
{
    g_interactionRequestHandler.store(handler, std::memory_order_release);
    (void)BeginInteractionRequest(
        handler == nullptr ? HdrSettingsInteractionState::unavailable :
                             HdrSettingsInteractionState::inactive);
}

bool RequestHdrSettingsInteraction(bool open) noexcept
{
    const InteractionDestination destination{
        g_interactionRequestHandler.load(std::memory_order_acquire),
        g_window.load(std::memory_order_acquire),
        g_windowThread.load(std::memory_order_acquire),
        g_interactionMessage.load(std::memory_order_acquire)};
    if (!destination.Valid())
    {
        (void)BeginInteractionRequest(HdrSettingsInteractionState::unavailable);
        return false;
    }
    const auto generation = BeginInteractionRequest(open ?
        HdrSettingsInteractionState::opening : HdrSettingsInteractionState::closing);
    if (GetCurrentThreadId() == destination.thread)
        return ApplyInteractionRequest(generation, open);
    return PostInteractionRequest(destination, generation, open);
}
HdrSettingsInteractionState ReadHdrSettingsInteractionState() noexcept
{
    return InteractionState(
        g_interactionStatus.load(std::memory_order_acquire));
}

bool HdrSettingsInputInstalled() noexcept
{
    return g_active.load(std::memory_order_acquire);
}

std::uint64_t HdrSettingsInputOverflowCount() noexcept
{
    return g_overflowCount.load(std::memory_order_acquire);
}

}  // namespace fo4hdr
