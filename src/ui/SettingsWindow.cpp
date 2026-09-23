// SPDX-License-Identifier: GPL-3.0-or-later
#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "Fallout4HDR/ui/HdrSettingsInput.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"
#include "Fallout4HDR/graphics/HdrRenderer.hpp"
#include "SettingsPanel.hpp"
#include "DisplayCalibrationPanel.hpp"
#include "LutProfilesPanel.hpp"
#include "../graphics/LiveSettings.hpp"
#include "../graphics/DisplayDiscovery.hpp"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
HdrSettings g_draft;
GradeProfileSelection g_profileSelection;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ImGuiContext* g_imgui{};
HWND g_window{};
std::atomic<HWND> g_inputWindow{};
DWORD g_thread{};
bool g_platform{}, g_renderer{}, g_ready{}, g_open{}, g_drawing{}, g_commit{};
bool g_hideOverlay{};
bool g_consumerReady{};
bool g_saveRequested{};
HdrSettings g_saveDraft;
std::uint32_t g_calibrationMode{};
std::atomic<std::uint64_t> g_inputGeneration{};
std::uint64_t g_profileRequest{};
bool g_profileLoad{};
DisplayCalibrationProfile g_profileDraft;
SettingsPersistenceState g_profileState{SettingsPersistenceState::unavailable};
LARGE_INTEGER g_frequency{}, g_previous{};

class ImGuiScope
{
public:
    explicit ImGuiScope(ImGuiContext* context, bool retiring = false) noexcept : previous_(ImGui::GetCurrentContext())
    {
        if (retiring && previous_ == context) previous_ = nullptr;
        ImGui::SetCurrentContext(context);
    }
    ~ImGuiScope() noexcept { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};

ImGuiKey NamedKey(WPARAM key) noexcept
{
    struct Range
    {
        WPARAM first;
        WPARAM last;
        ImGuiKey named;
        bool Contains(WPARAM value) const noexcept { return value >= first && value <= last; }
    };
    constexpr std::array ranges{
        Range{'0', '9', ImGuiKey_0}, Range{'A', 'Z', ImGuiKey_A},
        Range{VK_F1, VK_F12, ImGuiKey_F1}, Range{VK_NUMPAD0, VK_NUMPAD9, ImGuiKey_Keypad0}};
    for (const auto& range : ranges)
        if (range.Contains(key)) return static_cast<ImGuiKey>(range.named + key - range.first);
    struct Mapping { WPARAM native; ImGuiKey named; };
    constexpr std::array mappings{
        Mapping{VK_TAB, ImGuiKey_Tab}, Mapping{VK_LEFT, ImGuiKey_LeftArrow},
        Mapping{VK_RIGHT, ImGuiKey_RightArrow}, Mapping{VK_UP, ImGuiKey_UpArrow},
        Mapping{VK_DOWN, ImGuiKey_DownArrow}, Mapping{VK_PRIOR, ImGuiKey_PageUp},
        Mapping{VK_NEXT, ImGuiKey_PageDown}, Mapping{VK_HOME, ImGuiKey_Home},
        Mapping{VK_END, ImGuiKey_End}, Mapping{VK_INSERT, ImGuiKey_Insert},
        Mapping{VK_DELETE, ImGuiKey_Delete}, Mapping{VK_BACK, ImGuiKey_Backspace},
        Mapping{VK_SPACE, ImGuiKey_Space}, Mapping{VK_RETURN, ImGuiKey_Enter},
        Mapping{VK_ESCAPE, ImGuiKey_Escape}, Mapping{VK_OEM_7, ImGuiKey_Apostrophe},
        Mapping{VK_OEM_COMMA, ImGuiKey_Comma}, Mapping{VK_OEM_MINUS, ImGuiKey_Minus},
        Mapping{VK_OEM_PERIOD, ImGuiKey_Period}, Mapping{VK_OEM_2, ImGuiKey_Slash},
        Mapping{VK_OEM_1, ImGuiKey_Semicolon}, Mapping{VK_OEM_PLUS, ImGuiKey_Equal},
        Mapping{VK_OEM_4, ImGuiKey_LeftBracket}, Mapping{VK_OEM_5, ImGuiKey_Backslash},
        Mapping{VK_OEM_6, ImGuiKey_RightBracket}, Mapping{VK_OEM_3, ImGuiKey_GraveAccent},
        Mapping{VK_CAPITAL, ImGuiKey_CapsLock}, Mapping{VK_SCROLL, ImGuiKey_ScrollLock},
        Mapping{VK_NUMLOCK, ImGuiKey_NumLock}, Mapping{VK_SNAPSHOT, ImGuiKey_PrintScreen},
        Mapping{VK_PAUSE, ImGuiKey_Pause}, Mapping{VK_DECIMAL, ImGuiKey_KeypadDecimal},
        Mapping{VK_DIVIDE, ImGuiKey_KeypadDivide}, Mapping{VK_MULTIPLY, ImGuiKey_KeypadMultiply},
        Mapping{VK_SUBTRACT, ImGuiKey_KeypadSubtract}, Mapping{VK_ADD, ImGuiKey_KeypadAdd},
        Mapping{VK_LSHIFT, ImGuiKey_LeftShift}, Mapping{VK_RSHIFT, ImGuiKey_RightShift},
        Mapping{VK_LCONTROL, ImGuiKey_LeftCtrl}, Mapping{VK_RCONTROL, ImGuiKey_RightCtrl},
        Mapping{VK_LMENU, ImGuiKey_LeftAlt}, Mapping{VK_RMENU, ImGuiKey_RightAlt},
        Mapping{VK_LWIN, ImGuiKey_LeftSuper}, Mapping{VK_RWIN, ImGuiKey_RightSuper},
        Mapping{VK_APPS, ImGuiKey_Menu}};
    const auto found = std::ranges::find(mappings, key, &Mapping::native);
    return found == mappings.end() ? ImGuiKey_None : found->named;
}

void ModifierEvents(ImGuiIO& io, std::uint32_t modifiers) noexcept
{
    io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & 1) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (modifiers & 2) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (modifiers & 4) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (modifiers & 8) != 0);
}

void KeyboardEvent(ImGuiIO& io, const HdrSettingsInputMessage& message) noexcept
{
    auto key = NamedKey(message.key);
    if (message.key == VK_RETURN && (static_cast<std::uintptr_t>(message.details) & (1U << 24)))
        key = ImGuiKey_KeypadEnter;
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN);
}

bool MouseButtonEvent(ImGuiIO& io, const HdrSettingsInputMessage& message) noexcept
{
    struct Button { UINT down; UINT repeat; UINT up; int index; };
    constexpr std::array buttons{
        Button{WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONUP, 0},
        Button{WM_RBUTTONDOWN, WM_RBUTTONDBLCLK, WM_RBUTTONUP, 1},
        Button{WM_MBUTTONDOWN, WM_MBUTTONDBLCLK, WM_MBUTTONUP, 2},
        Button{WM_XBUTTONDOWN, WM_XBUTTONDBLCLK, WM_XBUTTONUP, 3}};
    for (const auto& button : buttons)
    {
        const std::array messages{button.down, button.repeat, button.up};
        if (std::ranges::find(messages, message.message) == messages.end()) continue;
        const auto index = button.index + (button.index == 3 && HIWORD(message.key) == XBUTTON2 ? 1 : 0);
        io.AddMouseButtonEvent(index, message.message != button.up);
        return true;
    }
    return false;
}

void MouseEvent(ImGuiIO& io, const HdrSettingsInputMessage& message) noexcept
{
    if (MouseButtonEvent(io, message)) return;
    if (message.message == WM_MOUSEMOVE)
        io.AddMousePosEvent(static_cast<float>(static_cast<short>(LOWORD(message.details))),
            static_cast<float>(static_cast<short>(HIWORD(message.details))));
    if (message.message == WM_MOUSEWHEEL || message.message == WM_MOUSEHWHEEL)
    {
        const auto delta = static_cast<float>(static_cast<short>(HIWORD(message.key))) / WHEEL_DELTA;
        io.AddMouseWheelEvent(message.message == WM_MOUSEHWHEEL ? delta : 0.0F,
            message.message == WM_MOUSEWHEEL ? delta : 0.0F);
    }
}

void InputEvent(ImGuiIO& io, const HdrSettingsInputMessage& message) noexcept
{
    ModifierEvents(io, message.modifiers);
    constexpr std::array keyboard{WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP};
    if (std::ranges::find(keyboard, message.message) != keyboard.end())
        KeyboardEvent(io, message);
    else if (message.message == WM_CHAR || message.message == WM_SYSCHAR)
        io.AddInputCharacterUTF16(static_cast<ImWchar16>(message.key));
    else if (message.message == WM_UNICHAR)
    {
        if (message.key <= 0x10FFFFU) io.AddInputCharacter(static_cast<unsigned>(message.key));
    }
    else if (message.message == WM_SETFOCUS) io.AddFocusEvent(true);
    else MouseEvent(io, message);
}


HdrSettingsInput* CurrentInput() noexcept
{
    return SettingsInputForGeneration(g_inputGeneration.load(std::memory_order_acquire));
}

bool BackendTuple(HWND window, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    return g_window == window && g_device.Get() == device && g_context.Get() == context;
}

void ClearBackend() noexcept
{
    ImGuiScope scoped(g_imgui, true);
    if (g_renderer) ImGui_ImplDX11_Shutdown();
    if (g_platform) ImGui_ImplWin32_Shutdown();
    if (g_imgui) ImGui::DestroyContext(g_imgui);
    g_imgui = nullptr;
    g_renderer = g_platform = g_ready = g_open = false;
    g_calibrationMode = 0;
    g_consumerReady = false;
    g_context.Reset();
    g_device.Reset();
    g_window = nullptr;
    g_inputGeneration.store(0, std::memory_order_release);
    g_inputWindow.store(nullptr, std::memory_order_release);
}

bool CreateBackendResources(HWND window, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!QueryPerformanceFrequency(&g_frequency)) return false;
    if (g_frequency.QuadPart <= 0) return false;
    if (!QueryPerformanceCounter(&g_previous)) return false;
    ImGuiScope scoped(nullptr);
    g_imgui = ImGui::CreateContext();
    if (!g_imgui) return false;
    ImGui::GetIO().IniFilename = nullptr;
    g_platform = ImGui_ImplWin32_Init(window);
    if (!g_platform) return false;
    g_renderer = ImGui_ImplDX11_Init(device, context);
    if (!g_renderer) return false;
    // Stock backend compilation and later buffer growth retain their existing limits.
    return ImGui_ImplDX11_CreateDeviceObjects();
}

bool PrepareBackend(HWND window, ID3D11Device* device, ID3D11DeviceContext* context)
{
    const auto generation = ReadGraphicsObservation().resourceGeneration;
    if (generation == 0) return false;
    if (!IsWindow(window)) return false;
    g_thread = GetCurrentThreadId();
    g_window = window;
    g_device = device;
    g_context = context;
    if (!CreateBackendResources(window, device, context)) return false;
    if (!PublishSettingsInputHost(window, generation)) return false;
    g_inputGeneration.store(generation, std::memory_order_release);
    g_inputWindow.store(window, std::memory_order_release);
    g_ready = true;
    (void)TryActivateSettingsInputHostOnCurrentThread();
    return true;
}

void DrainInput(HdrSettingsInput& input, const SettingsWindowActions& actions) noexcept
{
    std::array<HdrSettingsInputMessage, HdrSettingsInput::capacity> messages;
    const auto copied = input.Drain(messages);
    auto& io = ImGui::GetIO();
    if (copied.focusLost)
    {
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.AddFocusEvent(copied.focused);
        g_open = false;
        actions.focusLost();
    }
    for (std::size_t index = 0; index < copied.messages; ++index) InputEvent(io, messages[index]);
    if (copied.toggles % 2 != 0) g_open = !g_open;
}

bool BeginUiFrame() noexcept
{
    LARGE_INTEGER now{};
    RECT client{};
    if (!QueryPerformanceCounter(&now) || !GetClientRect(g_window, &client)) return false;
    const auto elapsed = static_cast<double>(now.QuadPart - g_previous.QuadPart) /
        static_cast<double>(g_frequency.QuadPart);
    g_previous = now;
    auto& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(client.right), static_cast<float>(client.bottom)};
    io.DeltaTime = static_cast<float>(std::clamp(elapsed, 0.001, 0.1));
    io.MouseDrawCursor = g_open;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    return true;
}

bool SameCalibration(const HdrSettings& value, const DisplayCalibrationProfile& draft) noexcept
{
    return value.displayPeakNits == draft.displayPeakNits &&
        value.uiPaperWhiteNits == draft.uiPaperWhiteNits && value.videoWhiteNits == draft.videoWhiteNits;
}

void RefreshDisplayView(SettingsWindowView& view, const SettingsWindowActions& actions) noexcept
{
    InvalidateDisplayCapabilities();
    (void)RefreshDisplayCapabilities(g_window);
    actions.refreshView(view);
}

void ConsumeCalibration(SettingsWindowView& view, const SettingsWindowActions& actions) noexcept
{
    const auto result = ReadSettingsPersistenceSnapshot();
    g_profileState = result.displayCalibrationState;
    if (!g_profileRequest || result.displayCalibrationGeneration != g_profileRequest) return;
    g_profileRequest = 0;
    if (!g_profileLoad || result.displayCalibrationState != SettingsPersistenceState::loaded) return;
    RefreshDisplayView(view, actions);
    if (!view.display.known || result.displayCalibration.monitorDevicePath != view.display.monitorDevicePath) return;
    if (!SameCalibration(g_draft, g_profileDraft)) return;
    g_draft.displayPeakNits = result.displayCalibration.displayPeakNits;
    g_draft.uiPaperWhiteNits = result.displayCalibration.uiPaperWhiteNits;
    g_draft.videoWhiteNits = result.displayCalibration.videoWhiteNits;
    g_commit = true;
}

bool CalibrationRequested(const DisplayCalibrationActions& requested, const SettingsWindowView& view) noexcept
{
    if (g_profileRequest) return false;
    if (!view.display.known) return false;
    return requested.save || requested.load;
}

void SubmitCalibration(bool save, const SettingsWindowView& view) noexcept
{
    const auto& identity = view.display.monitorDevicePath;
    g_profileLoad = !save;
    if (save)
        g_profileRequest = RequestDisplayCalibrationSave(identity.data(), view.activeSettings);
    else
    {
        g_profileDraft = {identity, g_draft.displayPeakNits, g_draft.uiPaperWhiteNits, g_draft.videoWhiteNits};
        g_profileRequest = RequestDisplayCalibrationLoad(identity.data());
    }
}

void RequestCalibration(const DisplayCalibrationActions& requested, SettingsWindowView& view,
    const SettingsWindowActions& actions) noexcept
{
    if (!CalibrationRequested(requested, view)) return;
    const auto identity = view.display.monitorDevicePath;
    RefreshDisplayView(view, actions);
    if (!view.display.known) return;
    if (identity != view.display.monitorDevicePath) return;
    SubmitCalibration(requested.save, view);
}

const char* ProfileStatusText(SettingsPersistenceState state) noexcept
{
    switch (state)
    {
    case SettingsPersistenceState::loading: return "Reading display profile...";
    case SettingsPersistenceState::saving: return "Saving display profile...";
    case SettingsPersistenceState::saved: return "Display profile saved.";
    case SettingsPersistenceState::loaded: return "Display profile read completed.";
    case SettingsPersistenceState::missing: return "No profile for this display.";
    case SettingsPersistenceState::failed: return "Display profile operation failed.";
    default: return nullptr;
    }
}

void DrawCalibration(SettingsWindowView& view, const SettingsWindowActions& callbacks, bool& changed)
{
    if (!ImGui::CollapsingHeader("Display calibration")) return;
    ConsumeCalibration(view, callbacks);
    std::array<char, 256> displayName{};
    (void)WideCharToMultiByte(CP_UTF8, 0, view.display.friendlyName.data(), -1,
        displayName.data(), static_cast<int>(displayName.size()), nullptr, nullptr);
    const DisplayCalibrationView display{displayName.data(), view.display.peakNits,
        view.display.fullFrameNits, view.display.sdrWhiteNits,
        view.display.known && view.display.monitorDevicePath[0] != L'\0',
        g_profileRequest != 0 || g_profileState == SettingsPersistenceState::unavailable,
        !view.lutPending && !view.liveSettingsError, ProfileStatusText(g_profileState)};
    if (ImGui::Button("Refresh display details")) RefreshDisplayView(view, callbacks);
    const auto result = DrawDisplayCalibrationPanel(g_draft, g_calibrationMode, display);
    RequestCalibration(result, view, callbacks);
    changed |= result.changed;
    g_hideOverlay = result.hideOverlay;
}

void DrawCurrentPanel(SettingsWindowView& view, const SettingsWindowActions& callbacks)
{
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowSize({std::min(440.0F, io.DisplaySize.x), std::min(900.0F, io.DisplaySize.y)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5F, io.DisplaySize.y * 0.5F}, ImGuiCond_FirstUseEver, {0.5F, 0.5F});
    SettingsPanelActions actions;
    g_hideOverlay = false;
    if (ImGui::Begin("Fallout4HDR", &g_open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
    {
        actions = DrawSettingsPanel(g_draft, view);
        if (ImGui::CollapsingHeader("LUT and ImageSpace profiles"))
            actions.request |= DrawLutProfilesPanel(g_draft, view.lutProfiles, g_profileSelection);
        DrawCalibration(view, callbacks, actions.request);
    }
    ImGui::End();
    if (actions.request) g_commit = true;
    if (actions.save) { g_saveDraft = g_draft; g_saveRequested = true; }
}

void DrawCurrentDiagnostics(const SettingsWindowView& view)
{
    if (!g_configuration.showDeveloperOverlay) return;
    if (ImGui::Begin("Fallout4HDR diagnostics", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextUnformatted(g_configuration.developerHdr10Reference ? "Native HDR10 reference" : "Production HDR");
        const auto evidence = ReadGraphicsObservation();
        ImGui::Text("Present observations: %llu; HDR submissions: %llu",
            static_cast<unsigned long long>(evidence.presentCount), static_cast<unsigned long long>(evidence.hdrFrameCount));
        ImGui::TextUnformatted(g_configuration.traceRenderPipeline ? "Pipeline trace enabled" : "Pipeline trace disabled");
        ImGui::TextUnformatted(view.rendererFailure);
    }
    ImGui::End();
}

void PrepareBackendOrClear(HWND window, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    try { if (!PrepareBackend(window, device, context)) ClearBackend(); }
    catch (...) { ClearBackend(); }
}

bool OnClosedUiThread() noexcept
{
    if (g_drawing) return false;
    return g_thread == GetCurrentThreadId();
}

bool NativeBackendArguments(HWND window, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (!window) return false;
    if (!device) return false;
    return context != nullptr;
}

bool EnableInputConsumer(bool interactive) noexcept
{
    if (!interactive) return true;
    if (g_consumerReady) return true;
    const auto generation = g_inputGeneration.load(std::memory_order_acquire);
    g_consumerReady = SetSettingsInputHostConsumerReady(generation, true);
    return g_consumerReady;
}

bool SaveRequestReady(const SettingsWindowView& view) noexcept
{
    if (!g_saveRequested) return false;
    if (g_commit) return false;
    if (view.liveSettingsError) return false;
    return true;
}

void SaveRequestedDraft(const SettingsWindowView& view) noexcept
{
    if (SaveRequestReady(view)) g_saveRequested = RequestHdrSettingsSave(g_saveDraft) == 0;
}

void DrawFramePanels(SettingsWindowView& view, const SettingsWindowActions& actions, bool interactive)
{
    if (g_open && interactive) DrawCurrentPanel(view, actions);
    else { g_hideOverlay = false; g_calibrationMode = 0; }
    DrawCurrentDiagnostics(view);
}

class UiFrame
{
public:
    explicit UiFrame(bool begun) noexcept : open_(begun) {}
    ~UiFrame() noexcept { if (open_) ImGui::EndFrame(); }
    void Render() { ImGui::Render(); open_ = false; }
private:
    bool open_;
};

bool DrawUiFrame(SettingsWindowView& view, const SettingsWindowActions& actions, HdrSettingsInput& input)
{
    DrainInput(input, actions);
    const auto interactive = g_configuration.productionRequested;
    if (!EnableInputConsumer(interactive)) return false;
    const auto begun = BeginUiFrame();
    UiFrame frame(begun);
    if (!begun) return false;
    DrawFramePanels(view, actions, interactive);
    frame.Render();
    SaveRequestedDraft(view);
    const auto& io = ImGui::GetIO();
    const auto open = g_open && interactive;
    input.SetCapture(open, io.WantCaptureKeyboard, io.WantCaptureMouse);
    return (open || g_configuration.showDeveloperOverlay) && !g_hideOverlay;
}

}  // namespace

void InitializeSettingsWindow(const HdrSettings& settings) noexcept
{
    g_draft = settings;
    g_commit = false;
}

void EnsureSettingsWindowBackend(bool requested, HWND window, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (!requested) { (void)ShutdownSettingsWindowBackend(); return; }
    if (g_ready && BackendTuple(window, device, context)) return;
    if (g_imgui)
        if (!ShutdownSettingsWindowBackend()) return;
    if (!NativeBackendArguments(window, device, context)) return;
    PrepareBackendOrClear(window, device, context);
}

bool ShutdownSettingsWindowBackend() noexcept
{
    if (!g_imgui) return true;
    if (g_thread != GetCurrentThreadId() || g_drawing) return false;
    const auto generation = g_inputGeneration.load(std::memory_order_acquire);
    // Refusal leaves the published consumer and its retained backend intact.
    if (generation && !SetSettingsInputHostConsumerReady(generation, false)) return false;
    RevokeSettingsInputHost();
    ClearBackend();
    return true;
}

bool SettingsWindowBackendReady() noexcept { return g_ready; }

SettingsInputActivation TryActivateSettingsWindowInput() noexcept
{
    const auto generation = g_inputGeneration.load(std::memory_order_acquire);
    const auto active = generation && TryActivateSettingsInputHostOnCurrentThread();
    const auto window = g_inputWindow.load(std::memory_order_acquire);
    return {generation != 0, active, GetCurrentThreadId(), window ? GetWindowThreadProcessId(window, nullptr) : 0};
}

bool DrawSettingsWindow(SettingsWindowView& view, const SettingsWindowActions& actions) noexcept
{
    if (!g_ready) return false;
    if (!OnClosedUiThread()) return false;
    auto* input = CurrentInput();
    if (!input) return false;
    if (!input->Matches(g_window, g_thread)) return false;
    ImGuiScope scoped(g_imgui);
    g_drawing = true;
    bool visible{};
    try { visible = DrawUiFrame(view, actions, *input); }
    catch (...) { input->SetCapture(false, false, false); }
    g_drawing = false;
    return visible;
}

bool SetSettingsWindowOpen(bool open) noexcept
{
    if (!g_ready) return false;
    if (!OnClosedUiThread()) return false;
    g_open = open;
    if (!open) g_calibrationMode = 0;
    return true;
}

bool SettingsWindowOpen() noexcept { return g_open; }
std::uint32_t SettingsCalibrationMode() noexcept { return g_calibrationMode; }
const HdrSettings& ReadSettingsDraft() noexcept { return g_draft; }
bool SettingsCommitRequested() noexcept { return g_commit; }
bool ConsumeSettingsCommitRequest() noexcept { return std::exchange(g_commit, false); }

}  // namespace fo4hdr
