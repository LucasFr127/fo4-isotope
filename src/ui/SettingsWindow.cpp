// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "SettingsPanel.hpp"
#include "DisplayCalibrationPanel.hpp"
#include "DisplayDiscovery.hpp"
#include "LiveSettings.hpp"
#include "passes/PresentationPass.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace fo4hdr {
namespace {

void DrawLutInspector(PresentationPass* presentation)
{
    if (!ImGui::CollapsingHeader("Native grading inputs")) return;
    const auto observation = presentation ? presentation->CopyLutInspector() : LutProfileDrawSnapshot{};
    ImGui::Text("LUT qualification: %s", observation.status);
    ImGui::Text("ImageSpace qualification: %s", observation.imageSpaceStatus);
    for (std::size_t slot = 0; slot < observation.currentLuts.size(); ++slot)
    {
        const auto& lut = observation.currentLuts[slot];
        const bool validated = (observation.validatedSlots & (1u << slot)) != 0;
        ImGui::Text("Slot %u: %s", static_cast<unsigned>(slot),
            validated ? lut.utf8Path.data() : "Unavailable");
    }
}

using Microsoft::WRL::ComPtr;
constexpr UINT outputCount = 8;
constexpr UINT classCapacity = 256;

template<class T>
struct SavedShader
{
    ComPtr<T> shader;
    std::array<ID3D11ClassInstance*, classCapacity> classes{};
    UINT count{classCapacity};

    ~SavedShader()
    {
        for (auto* instance : classes) if (instance) instance->Release();
    }

    template<class Getter>
    bool Capture(ID3D11DeviceContext* context, Getter getter) noexcept
    {
        (context->*getter)(shader.GetAddressOf(), classes.data(), &count);
        return count <= classCapacity;
    }

    template<class Setter>
    void Restore(ID3D11DeviceContext* context, Setter setter) noexcept
    {
        (context->*setter)(shader.Get(), classes.data(), count);
    }
};

// The stock renderer restores its normal IA/RS/PS/VS/GS state but drops HS/DS/CS,
// and restores VS b0 with the base API. This guard retains the omitted state and
// disables predication/output hazards before any overlay clear or native draw.
class OverlayState
{
public:
    explicit OverlayState(ID3D11DeviceContext1* context) noexcept : context_(context)
    {
        complete_ = CaptureShaders() && CaptureOutputs();
        context_->VSGetConstantBuffers1(0, 1, constants_.GetAddressOf(), &first_, &count_);
        complete_ = complete_ && first_ % 16 == 0 && count_ % 16 == 0 && count_ <= 4096;
        context_->GetPredication(predicate_.GetAddressOf(), &predicateValue_);
    }

    ~OverlayState() noexcept
    {
        if (!changed_) return;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->CSSetUnorderedAccessViews(0, outputCount, emptyUavs_.data(), counters_.data());
        RestoreOutputs();
        vertex_.Restore(context_, &ID3D11DeviceContext::VSSetShader);
        pixel_.Restore(context_, &ID3D11DeviceContext::PSSetShader);
        geometry_.Restore(context_, &ID3D11DeviceContext::GSSetShader);
        hull_.Restore(context_, &ID3D11DeviceContext::HSSetShader);
        domain_.Restore(context_, &ID3D11DeviceContext::DSSetShader);
        compute_.Restore(context_, &ID3D11DeviceContext::CSSetShader);
        auto* constants = constants_.Get();
        context_->VSSetConstantBuffers1(0, 1, &constants, &first_, &count_);
        context_->SetPredication(predicate_.Get(), predicateValue_);
    }

    bool Begin(ID3D11RenderTargetView* target) noexcept
    {
        if (!complete_) return false;
        counters_.fill(std::numeric_limits<UINT>::max());
        context_->SetPredication(nullptr, FALSE);
        context_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
            0, outputCount, emptyUavs_.data(), counters_.data());
        context_->CSSetUnorderedAccessViews(0, outputCount, emptyUavs_.data(), counters_.data());
        context_->OMSetRenderTargets(1, &target, nullptr);
        changed_ = true;
        return true;
    }

private:
    bool CaptureShaders() noexcept
    {
        const std::array captured{
            vertex_.Capture(context_, &ID3D11DeviceContext::VSGetShader),
            pixel_.Capture(context_, &ID3D11DeviceContext::PSGetShader),
            geometry_.Capture(context_, &ID3D11DeviceContext::GSGetShader),
            hull_.Capture(context_, &ID3D11DeviceContext::HSGetShader),
            domain_.Capture(context_, &ID3D11DeviceContext::DSGetShader),
            compute_.Capture(context_, &ID3D11DeviceContext::CSGetShader)};
        return std::ranges::all_of(captured, [](bool value) { return value; });
    }

    void CaptureTargets() noexcept
    {
        std::array<ID3D11RenderTargetView*, outputCount> targets{};
        context_->OMGetRenderTargets(outputCount, targets.data(), depth_.GetAddressOf());
        for (UINT index = 0; index < outputCount; ++index)
        {
            targets_[index].Attach(targets[index]);
            if (targets[index]) targetCount_ = index + 1;
        }
    }

    bool CaptureOutputs() noexcept
    {
        CaptureTargets();
        std::array<ID3D11UnorderedAccessView*, outputCount> om{};
        std::array<ID3D11UnorderedAccessView*, outputCount> cs{};
        context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, outputCount, om.data());
        context_->CSGetUnorderedAccessViews(0, outputCount, cs.data());
        bool valid = true;
        for (UINT index = 0; index < outputCount; ++index)
        {
            om_[index].Attach(om[index]);
            cs_[index].Attach(cs[index]);
            if (index < targetCount_ && om[index]) valid = false;
        }
        return valid;
    }

    void RestoreOutputs() noexcept
    {
        std::array<ID3D11RenderTargetView*, outputCount> targets{};
        std::array<ID3D11UnorderedAccessView*, outputCount> om{};
        std::array<ID3D11UnorderedAccessView*, outputCount> cs{};
        for (UINT index = 0; index < outputCount; ++index)
        {
            targets[index] = targets_[index].Get();
            om[index] = om_[index].Get();
            cs[index] = cs_[index].Get();
        }
        if (targetCount_ == outputCount)
            context_->OMSetRenderTargets(outputCount, targets.data(), depth_.Get());
        else
            context_->OMSetRenderTargetsAndUnorderedAccessViews(targetCount_, targets.data(), depth_.Get(),
                targetCount_, outputCount - targetCount_, om.data() + targetCount_, counters_.data());
        context_->CSSetUnorderedAccessViews(0, outputCount, cs.data(), counters_.data());
    }

    ID3D11DeviceContext1* context_;
    SavedShader<ID3D11VertexShader> vertex_;
    SavedShader<ID3D11PixelShader> pixel_;
    SavedShader<ID3D11GeometryShader> geometry_;
    SavedShader<ID3D11HullShader> hull_;
    SavedShader<ID3D11DomainShader> domain_;
    SavedShader<ID3D11ComputeShader> compute_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Predicate> predicate_;
    ComPtr<ID3D11DepthStencilView> depth_;
    std::array<ComPtr<ID3D11RenderTargetView>, outputCount> targets_;
    std::array<ComPtr<ID3D11UnorderedAccessView>, outputCount> om_;
    std::array<ComPtr<ID3D11UnorderedAccessView>, outputCount> cs_;
    std::array<ID3D11UnorderedAccessView*, outputCount> emptyUavs_{};
    std::array<UINT, outputCount> counters_{};
    UINT targetCount_{};
    UINT first_{};
    UINT count_{};
    BOOL predicateValue_{};
    bool complete_{};
    bool changed_{};
};

bool SameIdentity(IUnknown* left, IUnknown* right) noexcept
{
    if (!left || !right) return false;
    ComPtr<IUnknown> a;
    ComPtr<IUnknown> b;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) && a &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && b && a.Get() == b.Get();
}

bool MatchesNativeContext(ID3D11Device* device, ID3D11DeviceContext1* context) noexcept
{
    ComPtr<ID3D11Device> actual;
    ComPtr<ID3D11DeviceContext> immediate;
    context->GetDevice(&actual);
    device->GetImmediateContext(&immediate);
    return SameIdentity(device, actual.Get()) && SameIdentity(context, immediate.Get());
}

bool OffsetSupported(ID3D11Device* device) noexcept
{
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    return SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
        options.ConstantBufferOffsetting;
}

bool QualifiedContext(ID3D11Device* device, ID3D11DeviceContext1* context) noexcept
{
    if (!device || !context) return false;
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    return MatchesNativeContext(device, context) && OffsetSupported(device);
}

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

bool QualifiedConfiguration(const SettingsWindowConfiguration& configuration) noexcept
{
    if (!configuration.input || !configuration.live) return false;
    if (!configuration.width || !configuration.height) return false;
    constexpr auto maximum = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (configuration.width > maximum || configuration.height > maximum) return false;
    return configuration.input->Matches(configuration.window, GetCurrentThreadId());
}
class CurrentContext
{
public:
    explicit CurrentContext(ImGuiContext* selected) noexcept : previous_(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(selected);
    }
    ~CurrentContext() { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};

}  // namespace

struct SettingsWindow::State
{
    DWORD thread{GetCurrentThreadId()};
    SettingsWindowConfiguration configuration;
    DisplayDiscovery displayDiscovery;
    DisplayCalibrationSnapshot profileStatus;
    DisplayCalibrationProfile loadDraft;
    std::uint64_t profileGeneration{};
    bool loadingProfile{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> source;
    ImGuiContext* imgui{};
    PluginConfiguration editable;
    LARGE_INTEGER frequency{};
    LARGE_INTEGER previous{};
    std::uint64_t frame{};
    SettingsWindowOverlay publication;
    bool platform{};
    bool renderer{};
    std::uint32_t calibrationMode{};
    bool hideOverlay{};
    bool open{};
    bool drawing{};

    ~State() noexcept
    {
        displayDiscovery.Reset();
        if (!imgui) return;
        auto* old = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(imgui);
        configuration.input->SetCapture(false, false, false);
        if (renderer) ImGui_ImplDX11_Shutdown();
        if (platform) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(imgui);
        ImGui::SetCurrentContext(old == imgui ? nullptr : old);
    }

    bool PrepareSurface() noexcept
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = configuration.width;
        description.Height = configuration.height;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)) || !texture) return false;
        return PrepareViews();
    }

    bool PrepareViews() noexcept
    {
        if (FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)) || !target) return false;
        return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &source)) && source;
    }

    bool PrepareBackend()
    {
        auto* old = ImGui::GetCurrentContext();
        imgui = ImGui::CreateContext();
        if (!imgui) return false;
        ImGui::SetCurrentContext(old);
        const CurrentContext selected(imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = io.LogFilename = nullptr;
        io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
        io.FontDefault = io.Fonts->AddFontDefaultVector();
        ImGui::StyleColorsDark();
        if (!io.FontDefault) return false;
        platform = ImGui_ImplWin32_Init(configuration.window);
        if (!platform) return false;
        renderer = ImGui_ImplDX11_Init(device.Get(), context.Get());
        if (!renderer) return false;
        // The unmodified backend compiles its shaders here and can grow buffers
        // later; readiness is not shipping-policy or hot-path performance proof.
        return ImGui_ImplDX11_CreateDeviceObjects();
    }

    bool Prepare()
    {
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
        if (!QueryPerformanceCounter(&previous)) return false;
        return PrepareSurface() && PrepareBackend();
    }

    bool CanDraw(std::uint64_t requestedFrame, SettingsWindowScope scope) const noexcept
    {
        if (thread != GetCurrentThreadId() || drawing) return false;
        if (scope != SettingsWindowScope::qualifiedExclusive) return false;
        if (!configuration.input->Matches(configuration.window, thread)) return false;
        return requestedFrame != 0 && requestedFrame >= frame;
    }
    void DrainInput() noexcept
    {
        std::array<HdrSettingsInputMessage, HdrSettingsInput::capacity> messages;
        const auto snapshot = configuration.input->Drain(messages);
        auto& io = ImGui::GetIO();
        if (snapshot.focusLost)
        {
            io.ClearEventsQueue();
            io.ClearInputKeys();
            io.ClearInputMouse();
            io.AddFocusEvent(snapshot.focused);
            open = false;
        }
        for (std::size_t index = 0; index < snapshot.messages; ++index) InputEvent(io, messages[index]);
        if (snapshot.toggles % 2 != 0) open = !open;
    }

    bool BeginFrame() noexcept
    {
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) return false;
        const auto elapsed = static_cast<double>(now.QuadPart - previous.QuadPart) /
            static_cast<double>(frequency.QuadPart);
        previous = now;
        auto& io = ImGui::GetIO();
        io.DisplaySize = {static_cast<float>(configuration.width), static_cast<float>(configuration.height)};
        io.DeltaTime = static_cast<float>(std::clamp(elapsed, 0.001, 0.1));
        io.MouseDrawCursor = open;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    static DisplayCalibrationProfile ProfileFor(
        const DisplayCapabilities& display, const HdrSettings& settings) noexcept
    {
        return {display.monitorDevicePath, settings.displayPeakNits,
            settings.uiPaperWhiteNits, settings.videoWhiteNits};
    }

    bool LoadStillApplies(const DisplayCapabilities& display,
        const DisplayCalibrationProfile& profile) const noexcept
    {
        if (profile.monitorDevicePath != display.monitorDevicePath) return false;
        const auto current = ProfileFor(display, editable.display);
        if (current.displayPeakNits != loadDraft.displayPeakNits) return false;
        if (current.uiPaperWhiteNits != loadDraft.uiPaperWhiteNits) return false;
        return current.videoWhiteNits == loadDraft.videoWhiteNits;
    }

    DisplayCapabilities ResolveDisplay() noexcept
    {
        displayDiscovery.Reset();
        return displayDiscovery.Refresh(configuration.window);
    }

    void ApplyLoadedProfile(const DisplayCalibrationSnapshot& status) noexcept
    {
        if (!loadingProfile || status.state != SettingsPersistenceState::loaded) return;
        const auto display = ResolveDisplay();
        if (!display.known || !LoadStillApplies(display, status.profile)) return;
        auto candidate = editable;
        candidate.display.displayPeakNits = status.profile.displayPeakNits;
        candidate.display.uiPaperWhiteNits = status.profile.uiPaperWhiteNits;
        candidate.display.videoWhiteNits = status.profile.videoWhiteNits;
        if (configuration.live->Request(candidate)) editable = candidate;
    }

    bool OwnProfileCompletion(const DisplayCalibrationSnapshot& status) noexcept
    {
        if (!profileGeneration) return false;
        if (status.requestedGeneration != profileGeneration)
        {
            profileGeneration = 0;
            return false;
        }
        if (status.completedGeneration != profileGeneration) return false;
        profileGeneration = 0;
        return true;
    }

    void ConsumeProfile() noexcept
    {
        if (!configuration.persistence) return;
        const auto status = configuration.persistence->DisplayCalibrationStatus();
        if (status.state == SettingsPersistenceState::busy) return;
        profileStatus = status;
        if (!OwnProfileCompletion(status)) return;
        ApplyLoadedProfile(status);
    }

    bool CanRequestProfile(const DisplayCalibrationActions& actions,
        const DisplayCapabilities& display) const noexcept
    {
        if (!configuration.persistence || profileGeneration) return false;
        if (!display.known || display.monitorDevicePath[0] == L'\0') return false;
        return actions.saveProfile || actions.loadProfile;
    }

    void RequestProfile(const DisplayCalibrationActions& actions,
        const DisplayCapabilities& display, const PluginConfiguration& active) noexcept
    {
        if (!CanRequestProfile(actions, display)) return;
        const auto resolved = ResolveDisplay();
        if (!resolved.known || resolved.monitorDevicePath != display.monitorDevicePath) return;
        if (actions.saveProfile)
        {
            loadingProfile = false;
            profileGeneration = configuration.persistence->RequestDisplayCalibrationSave(
                ProfileFor(display, active.display));
        }
        else if (actions.loadProfile)
        {
            loadingProfile = true;
            loadDraft = ProfileFor(display, editable.display);
            profileGeneration = configuration.persistence->RequestDisplayCalibrationLoad(loadDraft);
        }
    }

    void DrawPanel()
    {
        const auto snapshot = configuration.live->Snapshot();
        
        const auto width = std::min(440.0F, static_cast<float>(configuration.width));
        const auto height = std::min(900.0F, static_cast<float>(configuration.height));
        ImGui::SetNextWindowSize({width, height}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({static_cast<float>(configuration.width) * 0.5F,
            static_cast<float>(configuration.height) * 0.5F}, ImGuiCond_FirstUseEver, {0.5F, 0.5F});
        SettingsPanelActions actions;
        hideOverlay = false;
        if (ImGui::Begin("Fallout4HDR", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
        {
            actions = DrawSettingsPanel(editable, snapshot);
            DrawLutInspector(configuration.presentation);
            if (ImGui::CollapsingHeader("Display calibration"))
            {
                const auto display = displayDiscovery.Refresh(configuration.window);
                ConsumeProfile();
                const DisplayCalibrationView view{display.known, display.monitorDevicePath[0] != L'\0',
                    display.friendlyName[0] ? display.friendlyName.data() : display.description.DeviceName,
                    display.description.MaxLuminance, display.description.MaxFullFrameLuminance,
                    display.sdrWhiteNits, configuration.persistence && profileGeneration == 0,
                    profileStatus.state};
                const auto calibration = DrawDisplayCalibrationPanel(editable.display, calibrationMode, view);
                if (calibration.refreshDisplay) displayDiscovery.Reset();
                RequestProfile(calibration, display, snapshot.active);
                actions.request |= calibration.changed;
                hideOverlay = calibration.hideOverlay;
            }
        }
        ImGui::End();
        if (actions.request) (void)configuration.live->Request(editable);
        if (actions.save) (void)configuration.live->RequestSave();
    }

    SettingsWindowOverlay RenderFrame(std::uint64_t requestedFrame, bool& frameOpen)
    {
        DrainInput();
        if (!BeginFrame()) return {};
        frameOpen = true;
        if (open) DrawPanel();
        ImGui::Render();
        frameOpen = false;
        const auto& io = ImGui::GetIO();
        configuration.input->SetCapture(open, io.WantCaptureKeyboard, io.WantCaptureMouse);
        if (!open)
        {
            calibrationMode = 0;
            return {};
        }
        if (hideOverlay)
            return {nullptr, requestedFrame, configuration.width, configuration.height, calibrationMode};
        if (!DrawOverlay()) return {};
        return {source.Get(), requestedFrame, configuration.width, configuration.height, calibrationMode};
    }
    bool DrawOverlay()
    {
        OverlayState saved(context.Get());
        if (!saved.Begin(target.Get())) return false;
        constexpr float transparent[4]{};
        context->ClearRenderTargetView(target.Get(), transparent);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return true;
    }
};

SettingsWindow::SettingsWindow() noexcept = default;
SettingsWindow::~SettingsWindow() noexcept { (void)Retire(); }

bool SettingsWindow::Initialize(ID3D11Device* device, ID3D11DeviceContext1* context,
    SettingsWindowConfiguration configuration) noexcept
{
    if (state_) return false;
    if (!QualifiedConfiguration(configuration)) return false;
    if (!QualifiedContext(device, context)) return false;
    try
    {
        auto candidate = std::make_unique<State>();
        candidate->configuration = configuration;
        candidate->editable = configuration.live->Snapshot().requested;
        candidate->device = device;
        candidate->context = context;
        if (!candidate->Prepare()) return false;
        state_ = std::move(candidate);
        return true;
    }
    catch (...) { return false; }
}

SettingsWindowOverlay SettingsWindow::Draw(std::uint64_t frame, SettingsWindowScope scope) noexcept
{
    if (!state_) return {};
    if (!state_->CanDraw(frame, scope)) return {};
    if (frame == state_->frame) return state_->publication;
    state_->publication = {};
    state_->drawing = true;
    SettingsWindowOverlay overlay;
    bool frameOpen{};
    try
    {
        const CurrentContext selected(state_->imgui);
        overlay = state_->RenderFrame(frame, frameOpen);
    }
    catch (...)
    {
        const CurrentContext selected(state_->imgui);
        if (frameOpen) ImGui::EndFrame();
        state_->configuration.input->SetCapture(false, false, false);
    }
    if (overlay.frame == 0) state_->calibrationMode = 0;
    state_->frame = frame;
    state_->publication = overlay;
    state_->drawing = false;
    return overlay;
}

bool SettingsWindow::Retire() noexcept
{
    if (!state_) return true;
    if (state_->thread != GetCurrentThreadId() || state_->drawing) return false;
    state_.reset();
    return true;
}

}  // namespace fo4hdr
