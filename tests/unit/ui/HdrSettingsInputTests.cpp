// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "Fallout4HDR/ui/HdrSettingsInput.hpp"
#include "TestHarness.hpp"
#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "../config/HdrSettingsTestSupport.hpp"
#include <string_view>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <thread>

namespace fo4hdr::tests {

void TestHdrShortcutInput()
{
    HdrSettingsShortcutLatch latch{};
    const HdrSettingsHotkey altH{'H', 2U};
    FO4HDR_CHECK(!latch.KeyDown(altH, 'H', 0U, false));
    FO4HDR_CHECK(!latch.KeyUp('H'));
    FO4HDR_CHECK(!latch.KeyDown(altH, 'H', 3U, false));
    FO4HDR_CHECK(!latch.KeyDown(altH, 'J', 2U, false));
    FO4HDR_CHECK(!latch.KeyDown(altH, 'H', 2U, true));
    FO4HDR_CHECK(!latch.KeyUp('H'));
    FO4HDR_CHECK(latch.KeyDown(altH, 'H', 2U, false));
    FO4HDR_CHECK(!latch.KeyDown(altH, 'H', 2U, true));
    FO4HDR_CHECK(!latch.KeyUp(VK_MENU));
    FO4HDR_CHECK(latch.KeyUp('H'));
    FO4HDR_CHECK(!latch.KeyUp('H'));

    const HdrSettingsHotkey single{VK_F10, 0U};
    FO4HDR_CHECK(latch.KeyDown(single, VK_F10, 0U, false));
    FO4HDR_CHECK(latch.KeyUp(VK_F10));
    FO4HDR_CHECK(!latch.KeyDown(single, VK_F10, 2U, false));
    FO4HDR_CHECK(!latch.KeyUp(VK_F10));
    const HdrSettingsHotkey combined{'K', 5U};
    FO4HDR_CHECK(latch.KeyDown(combined, 'K', 5U, false));
    latch.Reset(); // focus loss, rebinding, or capture entry cancels release
    FO4HDR_CHECK(!latch.KeyUp('K'));

    HdrSettingsCapturedGestureFilter filter{};
    constexpr std::uintptr_t scan = 0x39U << 16U;
    FO4HDR_CHECK(filter.Suppress(WM_KEYDOWN, VK_SPACE, scan, {true, true}));
    FO4HDR_CHECK(filter.Suppress(WM_KEYDOWN, VK_SPACE,
        scan | (1ULL << 30U), {false, true}));
    FO4HDR_CHECK(filter.Suppress(WM_CHAR, ' ', scan, {false, true}));
    FO4HDR_CHECK(filter.Suppress(WM_KEYUP, VK_SPACE, scan, {false, false}));
    FO4HDR_CHECK(filter.Suppress(WM_CHAR, ' ', scan, {false, false}));
    FO4HDR_CHECK(!filter.Suppress(WM_KEYDOWN, VK_SPACE, scan, {false, false}));
    FO4HDR_CHECK(!filter.Suppress(WM_CHAR, ' ', scan, {false, false}));
    FO4HDR_CHECK(!filter.Suppress(WM_KEYUP, VK_SPACE, scan, {false, false}));
    FO4HDR_CHECK(filter.Suppress(WM_SYSKEYDOWN, 'H', scan, {true, true}));
    FO4HDR_CHECK(filter.Suppress(WM_SYSCHAR, 'h', scan, {false, true}));
    filter.Reset();
    FO4HDR_CHECK(!filter.Suppress(WM_SYSKEYUP, 'H', scan, {false, false}));
    FO4HDR_CHECK(!filter.Suppress(WM_SYSCHAR, 'h', scan, {false, false}));
}

LRESULT CALLBACK ShortcutTestWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    constexpr std::array messages{WM_CHAR, WM_SYSCHAR, WM_UNICHAR,
        WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP};
    if (std::ranges::find(messages, message) != messages.end())
        return 42;
    return DefWindowProcW(window, message, wParam, lParam);
}

unsigned g_shortcutOpenRequests{};
unsigned g_shortcutCloseRequests{};

[[nodiscard]] std::uint32_t ShortcutTestModifiers() noexcept
{
    return ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ?
            kSettingsHotkeyControl : 0U) |
        ((GetKeyState(VK_MENU) & 0x8000) != 0 ? kSettingsHotkeyAlt : 0U) |
        ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ?
            kSettingsHotkeyShift : 0U) |
        (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0 ?
            8U : 0U);
}

bool RecordShortcutInteraction(bool open) noexcept
{
    ++(open ? g_shortcutOpenRequests : g_shortcutCloseRequests);
    return true;
}

struct ShortcutTestScope
{
    ShortcutTestScope(HINSTANCE module, const wchar_t* name) noexcept :
        instance(module), className(name)
    {}

    HINSTANCE instance{};
    const wchar_t* className{};
    HWND window{};
    std::array<BYTE, 256> savedKeys{};
    bool classRegistered{};
    bool keyboardSaved{};

    ~ShortcutTestScope()
    {
        SetHdrSettingsInputCapture(false, false, false);
        CancelHdrSettingsHotkeyCapture();
        ShutdownHdrSettingsInput();
        RegisterHdrSettingsInteractionRequestHandler(nullptr);
        FO4HDR_CHECK(ReadHdrSettingsInteractionState() ==
                HdrSettingsInteractionState::unavailable);
        SetHdrSettingsHotkey({});
        g_shortcutOpenRequests = 0;
        g_shortcutCloseRequests = 0;
        if (keyboardSaved)
            FO4HDR_CHECK(SetKeyboardState(savedKeys.data()));
        if (window != nullptr) FO4HDR_CHECK(DestroyWindow(window));
        if (classRegistered)
            FO4HDR_CHECK(UnregisterClassW(className, instance));
    }
};

[[nodiscard]] bool ConfigureShortcutModifiers(bool alt)
{
    std::array<BYTE, 256> keys{};
    if (alt) keys[VK_MENU] = 0x80;
    const auto keyboardSet = SetKeyboardState(keys.data()) != FALSE;
    FO4HDR_CHECK(keyboardSet);
    if (!keyboardSet) return false;
    const auto expectedModifiers = alt ? kSettingsHotkeyAlt : 0U;
    const auto modifiers = ShortcutTestModifiers();
    FO4HDR_CHECK(modifiers == expectedModifiers);
    if (modifiers != expectedModifiers) return false;
    return true;
}

[[nodiscard]] bool TestShortcutChord(HWND window, bool alt)
{
    constexpr LPARAM scan = 0x23U << 16U;
    constexpr LPARAM released = scan | (3ULL << 30U);
    constexpr LPARAM repeated = scan | (1ULL << 30U);
    if (!ConfigureShortcutModifiers(alt)) return false;
    const auto expectedModifiers = alt ? kSettingsHotkeyAlt : 0U;
    SetHdrSettingsHotkey({'H', expectedModifiers});
    SetHdrSettingsInputCapture(false, false, false);
    const UINT down = alt ? WM_SYSKEYDOWN : WM_KEYDOWN;
    const UINT up = alt ? WM_SYSKEYUP : WM_KEYUP;
    const UINT character = alt ? WM_SYSCHAR : WM_CHAR;
    FO4HDR_CHECK(SendMessageW(window, down, 'H', scan) == 0);
    FO4HDR_CHECK(SendMessageW(window, down, 'H', repeated) == 0);
    FO4HDR_CHECK(SendMessageW(window, character, 'h', scan) == 0);
    FO4HDR_CHECK(SendMessageW(window, up, 'H', released) == 0);
    FO4HDR_CHECK(SendMessageW(window, character, 'h', scan) == 0);

    // A fresh unowned gesture on the same physical key clears its marker.
    SetHdrSettingsHotkey({'J', 0U});
    FO4HDR_CHECK(SendMessageW(window, down, 'H', scan) == 42);
    FO4HDR_CHECK(SendMessageW(window, character, 'h', scan) == 42);
    FO4HDR_CHECK(SendMessageW(window, up, 'H', released) == 42);

    SetHdrSettingsInputCapture(true, false, false);
    BeginHdrSettingsHotkeyCapture();
    FO4HDR_CHECK(SendMessageW(window, down, 'H', scan) == 0);
    FO4HDR_CHECK(SendMessageW(window, up, 'H', released) == 0);
    FO4HDR_CHECK(SendMessageW(window, character, 'h', scan) == 0);
    HdrSettingsHotkey captured{};
    FO4HDR_CHECK(ConsumeHdrSettingsHotkeyCapture(captured));
    FO4HDR_CHECK(captured.virtualKey == 'H');
    FO4HDR_CHECK(captured.modifiers == (alt ? kSettingsHotkeyAlt : 0U));
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    FO4HDR_CHECK(SendMessageW(window, character, 'h', scan) == 42);
    return true;
}

void TestFunctionKeyShortcut(HWND window)
{
    std::array<BYTE, 256> keys{};
    const auto keyboardSet = SetKeyboardState(keys.data()) != FALSE;
    FO4HDR_CHECK(keyboardSet);
    if (!keyboardSet) return;
    const auto modifiers = ShortcutTestModifiers();
    FO4HDR_CHECK(modifiers == 0);
    if (modifiers != 0) return;

    RegisterHdrSettingsInteractionRequestHandler(&RecordShortcutInteraction);
    FO4HDR_CHECK(ReadHdrSettingsInteractionState() ==
        HdrSettingsInteractionState::inactive);
    SetHdrSettingsHotkey({VK_F10, 0U});
    SetHdrSettingsInputCapture(false, false, false);
    constexpr LPARAM f10Scan = 0x44U << 16U;
    constexpr LPARAM f10Repeat = f10Scan | (1ULL << 30U);
    constexpr LPARAM f10Released = f10Scan | (3ULL << 30U);
    FO4HDR_CHECK(SendMessageW(window, WM_SYSKEYDOWN, VK_F10, f10Scan) == 0);
    FO4HDR_CHECK(SendMessageW(window, WM_SYSKEYDOWN, VK_F10, f10Repeat) == 0);
    FO4HDR_CHECK(g_shortcutOpenRequests == 0);
    FO4HDR_CHECK(!ConsumeHdrSettingsToggle());
    FO4HDR_CHECK(SendMessageW(window, WM_SYSKEYUP, VK_F10, f10Released) == 0);
    FO4HDR_CHECK(g_shortcutOpenRequests == 1);
    FO4HDR_CHECK(g_shortcutCloseRequests == 0);
    FO4HDR_CHECK(ReadHdrSettingsInteractionState() ==
        HdrSettingsInteractionState::active);
    FO4HDR_CHECK(ConsumeHdrSettingsToggle());
    FO4HDR_CHECK(!ConsumeHdrSettingsToggle());

    SetHdrSettingsInputCapture(true, false, false);
    FO4HDR_CHECK(SendMessageW(window, WM_SYSKEYDOWN, VK_F10, f10Scan) == 0);
    FO4HDR_CHECK(SendMessageW(window, WM_SYSKEYUP, VK_F10, f10Released) == 0);
    FO4HDR_CHECK(g_shortcutOpenRequests == 1);
    FO4HDR_CHECK(g_shortcutCloseRequests == 1);
    FO4HDR_CHECK(ReadHdrSettingsInteractionState() ==
        HdrSettingsInteractionState::inactive);
    FO4HDR_CHECK(ConsumeHdrSettingsToggle());
    FO4HDR_CHECK(!ConsumeHdrSettingsToggle());
}

void RunHdrShortcutWindowProcedureTest()
{
    const auto instance = GetModuleHandleW(nullptr);
    ShortcutTestScope scope{instance, L"Fallout4HDR.ShortcutTest"};
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ShortcutTestWindow;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = scope.className;
    const auto registered = RegisterClassW(&windowClass);
    FO4HDR_CHECK(registered != 0);
    if (!registered) return;
    scope.classRegistered = true;
    const auto window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    FO4HDR_CHECK(window != nullptr);
    if (!window) return;
    scope.window = window;
    scope.keyboardSaved = GetKeyboardState(scope.savedKeys.data()) != FALSE;
    FO4HDR_CHECK(scope.keyboardSaved);
    if (!scope.keyboardSaved) return;
    FO4HDR_CHECK(InitializeHdrSettingsInput(window));
    if (!HdrSettingsInputInstalled()) return;
    for (bool alt : {false, true})
        if (!TestShortcutChord(window, alt)) return;

    TestFunctionKeyShortcut(window);
}

void TestHdrShortcutWindowProcedure()
{
    // Keep this HWND owner's input-state table independent of keyboard
    // messages consumed by earlier tests on the runner thread.
    std::jthread testThread(RunHdrShortcutWindowProcedureTest);
}

void TestHotkeyFormatting()
{
    for (std::uint32_t vk = 0; vk < 256; ++vk)
        for (std::uint32_t modifiers = 0; modifiers < 8; ++modifiers)
        {
            const HdrSettingsHotkey original{vk, modifiers};
            if (!IsSupportedHdrSettingsHotkey(original)) continue;
            const auto name = FormatHdrSettingsHotkey(original);
            HdrSettingsHotkey parsed{};
            FO4HDR_CHECK(ParseHdrSettingsHotkey(name.data(), parsed));
            FO4HDR_CHECK(parsed == original);
        }
    FO4HDR_CHECK(std::string_view(FormatHdrSettingsHotkey({}).data()) == "Alt+H");
    for (const auto value : {"-0.5", "-2", "nan", "65536"})
        FO4HDR_CHECK(!ParsePluginConfiguration(
            LegacyProductionConfiguration() + "\n[Interface]\nWindowX=" +
            value + "\nWindowY=0\n").valid);
    FO4HDR_CHECK(ParsePluginConfiguration(
        LegacyProductionConfiguration() +
        "\n[Interface]\nWindowX=100\nWindowY=200\n").valid);
}

}  // namespace fo4hdr::tests
