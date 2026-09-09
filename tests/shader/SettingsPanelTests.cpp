// SPDX-License-Identifier: GPL-3.0-or-later

#include "SettingsPanel.hpp"
#include "DisplayCalibrationPanel.hpp"
#include "DisplayDiscovery.hpp"
#include "LiveSettings.hpp"
#include "NativeSettingsHost.hpp"
#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "ShippingPixelShaderTests.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace fo4hdr::tests {
namespace {

class SettingsFrameContext
{
public:
    SettingsFrameContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {640, 1000};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels{};
        int width{};
        int height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        FO4HDR_CHECK(pixels != nullptr);
        FO4HDR_CHECK(width > 0);
        FO4HDR_CHECK(height > 0);
    }

    ~SettingsFrameContext() { ImGui::DestroyContext(); }
};

void DispatchPanelActions(SettingsPanelActions actions, LiveSettings& live,
    const PluginConfiguration& editable)
{
    if (actions.request)
        FO4HDR_CHECK(live.Request(editable) == LiveSettingsRequestStatus::unavailable);
    if (actions.save) FO4HDR_CHECK(!live.RequestSave());
}

void CheckDrawData()
{
    const auto* data = ImGui::GetDrawData();
    FO4HDR_CHECK(data != nullptr);
    if (!data) return;
    FO4HDR_CHECK(data->CmdListsCount > 0);
    FO4HDR_CHECK(data->TotalVtxCount > 0);
    FO4HDR_CHECK(data->TotalIdxCount > 0);
}

void RenderSettingsFrame(OperatorLutAlgorithm algorithm, LiveSettings& live)
{
    PluginConfiguration editable;
    editable.algorithm = algorithm;
    const auto before = editable;
    auto snapshot = live.Snapshot();
    snapshot.requested = editable;
    snapshot.active = editable;
    // Status is host-fed; the dispatcher below still has no active renderer.
    if (algorithm != OperatorLutAlgorithm::aces2)
    {
        snapshot.available = true;
        snapshot.pending = true;
        snapshot.persistence.state = SettingsPersistenceState::busy;
        snapshot.error = LiveSettingsError::workerBusy;
    }
    if (algorithm == OperatorLutAlgorithm::gt7)
    {
        snapshot.active.algorithm = static_cast<OperatorLutAlgorithm>(255);
        snapshot.error = LiveSettingsError::applyRejected;
        snapshot.persistence.state = SettingsPersistenceState::failed;
        snapshot.saveRejected = true;
    }
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({400, 900}, ImGuiCond_Always);
    if (ImGui::Begin("HDR settings", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const auto actions = DrawSettingsPanel(editable, snapshot);
        FO4HDR_CHECK(!actions.request);
        FO4HDR_CHECK(!actions.save);
        DispatchPanelActions(actions, live, editable);
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > vertices);
    }
    ImGui::End();
    ImGui::Render();
    CheckDrawData();
    FO4HDR_CHECK(editable == before);
}

void RenderCalibrationFrame()
{
    HdrSettings editable;
    editable.displayPeakNits = 900.0;
    editable.uiPaperWhiteNits = 150.0;
    editable.videoWhiteNits = 175.0;
    const auto before = editable;
    std::uint32_t mode = 11;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({400, 900}, ImGuiCond_Always);
    if (ImGui::Begin("Display calibration", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const DisplayCalibrationView display{true, true, L"Authored display", 1200, 400, 203,
            true, SettingsPersistenceState::missing};
        const auto actions = DrawDisplayCalibrationPanel(editable, mode, display);
        FO4HDR_CHECK(!actions.changed);
        FO4HDR_CHECK(!actions.hideOverlay);
        FO4HDR_CHECK(!actions.refreshDisplay);
        FO4HDR_CHECK(!actions.saveProfile && !actions.loadProfile);
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > vertices);
    }
    ImGui::End();
    ImGui::Render();
    CheckDrawData();
    FO4HDR_CHECK(editable == before);
    FO4HDR_CHECK(mode == 11);
}

thread_local unsigned forwardedToggles{};
constexpr LPARAM toggleScan = 0x44 << 16;

LRESULT CALLBACK SettingsTestWindowProcedure(HWND window, UINT message, WPARAM key, LPARAM details) noexcept
{
    if (key == VK_F10)
    {
        if (message == WM_KEYDOWN || message == WM_KEYUP) ++forwardedToggles;
    }
    return DefWindowProcW(window, message, key, details);
}

class OwnedSettingsWindow
{
public:
    OwnedSettingsWindow()
    {
        WNDCLASSW description{};
        description.lpfnWndProc = SettingsTestWindowProcedure;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = L"Fallout4HDR.SettingsBackendTest";
        atom_ = RegisterClassW(&description);
        FO4HDR_CHECK(atom_ != 0);
        if (!atom_) return;
        window_ = CreateWindowExW(0, description.lpszClassName, L"HDR settings test",
            WS_OVERLAPPEDWINDOW, 0, 0, 680, 1040, nullptr, nullptr, description.hInstance, nullptr);
        FO4HDR_CHECK(window_ != nullptr);
        if (!window_) return;
        FO4HDR_CHECK(PublishSettingsInputHost(window_, 1));
        FO4HDR_CHECK(TryActivateSettingsInputHostOnCurrentThread());
        input = SettingsInputForGeneration(1);
        FO4HDR_CHECK(input != nullptr);
    }

    ~OwnedSettingsWindow()
    {
        RevokeSettingsInputHost();
        if (input) (void)input->Stop();
        if (window_) (void)DestroyWindow(window_);
        if (atom_) (void)UnregisterClassW(L"Fallout4HDR.SettingsBackendTest", GetModuleHandleW(nullptr));
    }

    HWND Window() const noexcept { return window_; }
    HdrSettingsInput* input{};
private:
    HWND window_{};
    ATOM atom_{};
};

void CheckOverlayPixels(const gpu::Device& device, const SettingsWindowOverlay& overlay)
{
    FO4HDR_CHECK(overlay.source != nullptr);
    if (!overlay.source) return;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    overlay.source->GetResource(&resource);
    gpu::CheckHresult(resource.As(&texture), "Settings overlay texture");
    const auto pixels = gpu::detail::ReadScene(device, texture.Get());
    bool contribution{};
    for (const auto& pixel : pixels)
    {
        FO4HDR_CHECK(std::isfinite(pixel.alpha) && pixel.alpha >= 0 && pixel.alpha <= 1);
        FO4HDR_CHECK(std::isfinite(pixel.red) && pixel.red >= 0);
        FO4HDR_CHECK(std::isfinite(pixel.green) && pixel.green >= 0);
        FO4HDR_CHECK(std::isfinite(pixel.blue) && pixel.blue >= 0);
        contribution |= pixel.alpha > 0;
    }
    FO4HDR_CHECK(contribution);
}

void CheckCapturedText(HdrSettingsInput& input, HWND window)
{
    constexpr LPARAM scan = 0x1E << 16;
    input.SetCapture(true, true, false);
    FO4HDR_CHECK(input.Observe(window, WM_KEYDOWN, 'A', scan));
    FO4HDR_CHECK(!SetSettingsInputHostConsumerReady(1, false));
    FO4HDR_CHECK(input.Observe(window, WM_CHAR, 'A', scan));
    input.SetCapture(false, false, false);
    FO4HDR_CHECK(input.Observe(window, WM_KEYUP, 'A', scan | (1ULL << 31)));
    std::array<HdrSettingsInputMessage, HdrSettingsInput::capacity> messages;
    const auto snapshot = input.Drain(messages);
    FO4HDR_CHECK(snapshot.messages == 3);
    FO4HDR_CHECK(messages[1].message == WM_CHAR && messages[1].key == 'A');
}
class SettingsHostInputFile
{
public:
    SettingsHostInputFile()
    {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
            ("Fallout4HDR-settings-host-" + std::to_string(token));
        if (!std::filesystem::create_directory(directory_))
            throw std::runtime_error("Cannot create settings host directory");
        std::ofstream output(Path(), std::ios::binary | std::ios::trunc);
        output << "[Output]\nDisplayPeakNits=900\nUiPaperWhiteNits=150\nVideoWhiteNits=175\n"
                  "[ToneMapper]\nAlgorithm=GT7\n";
        output.close();
        if (!output) throw std::runtime_error("Cannot write settings host input");
    }

    ~SettingsHostInputFile()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path Path() const { return directory_ / "settings.ini"; }

private:
    std::filesystem::path directory_;
};

std::unique_ptr<OperatorLutBuildResult> PrepareSettingsHostTable(const PluginConfiguration& initial)
{
    auto prepared = std::make_unique<OperatorLutBuildResult>();
    prepared->request = {initial.algorithm, initial.display.displayPeakNits, 1,
        ActiveToneMapperParameters(initial.algorithm, initial.display.toneMapperParameters)};
    prepared->data = BuildOperatorLut(initial.algorithm, initial.display.displayPeakNits,
        prepared->request.parameters, {});
    gpu::CheckHresult(prepared->data.IsValid() ? S_OK : E_FAIL, "Settings host table");
    return prepared;
}

void RequireSettingsHost(bool accepted, std::string_view operation)
{
    gpu::CheckHresult(accepted ? S_OK : E_FAIL, operation);
}

void CheckSettingsHostResize(NativeSettingsHost& host, ScenePassRenderer& scenes,
    const gpu::Device& gpu, const NativeSettingsHostConfiguration& configuration)
{
    auto& output = *configuration.presentation;
    const auto window = configuration.window;

    RequireSettingsHost(host.SuspendSurfaceForResize(), "Settings host suspend");
    RequireSettingsHost(output.Retire(), "Settings host output retire");
    RequireSettingsHost(scenes.RetireForResize(), "Settings host scene retire");
    RECT previous{};
    RequireSettingsHost(GetWindowRect(window, &previous) != FALSE, "Settings host old window");
    RequireSettingsHost(SetWindowPos(window, nullptr, 0, 0,
        previous.right - previous.left + 2, previous.bottom - previous.top + 2,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Settings host window resize");
    RECT client{};
    RequireSettingsHost(GetClientRect(window, &client) != FALSE, "Settings host new client");
    const auto width = static_cast<UINT>(client.right);
    const auto height = static_cast<UINT>(client.bottom);
    PresentationConfiguration active;
    RequireSettingsHost(host.ReadActiveOutputConfiguration(width, height, scenes, active),
        "Settings host active output tuple");
    RequireSettingsHost(scenes.ReinitializeBorrowed(gpu.device.Get(), gpu.context.Get(),
        {width, height, active.whites}), "Settings host scene rebuild");
    std::destroy_at(&output);
    std::construct_at(&output);
    RequireSettingsHost(output.Initialize(gpu.device.Get(), gpu.context.Get(),
        active), "Settings host output rebuild");
    RequireSettingsHost(host.ResumeAfterResize(width, height, PresentationScope::qualifiedExclusive), "Settings host active reapply");
    FO4HDR_CHECK(!host.CommitClosedFrame(PresentationScope::qualifiedExclusive));
    (void)SendMessageW(window, WM_SETFOCUS, 0, 0);
    (void)SendMessageW(window, WM_KEYDOWN, VK_F10, toggleScan);
    (void)SendMessageW(window, WM_KEYUP, VK_F10, toggleScan | (1ULL << 31));
    const auto overlay = host.Window()->Draw(3, SettingsWindowScope::qualifiedExclusive);
    FO4HDR_CHECK(overlay.width == width && overlay.height == height);
    FO4HDR_CHECK(overlay.calibrationMode == 0);
    CheckOverlayPixels(gpu, overlay);
}

void RenderOwnedBackend()
{
    OwnedSettingsWindow window;
    if (!window.Window() || !window.input) return;
    (void)SendMessageW(window.Window(), WM_KEYDOWN, VK_F10, toggleScan);
    (void)SendMessageW(window.Window(), WM_KEYUP, VK_F10, toggleScan | (1ULL << 31));
    FO4HDR_CHECK(forwardedToggles == 2);
    CheckCapturedText(*window.input, window.Window());
    DisplayDiscovery discovery;
    FO4HDR_CHECK(!discovery.Refresh(nullptr).known);
    const auto display = discovery.Refresh(window.Window());
    FO4HDR_CHECK(!display.known || display.description.Monitor != nullptr);
    discovery.Reset();
    FO4HDR_CHECK(!discovery.Refresh(nullptr).known);
    RECT client{};
    FO4HDR_CHECK(GetClientRect(window.Window(), &client));
    const auto device = gpu::CreateWarpDevice();
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
    gpu::CheckHresult(device.context.As(&context), "Settings Context1");
    const SettingsHostInputFile inputFile;
    const auto loaded = LoadPluginConfiguration(inputFile.Path());
    RequireSettingsHost(loaded.status == PluginConfigurationStatus::loaded,
        "Settings host loaded tuple");
    auto prepared = PrepareSettingsHostTable(loaded.value);
    const LayerWhiteConfiguration whites{static_cast<float>(loaded.value.display.uiPaperWhiteNits),
        static_cast<float>(loaded.value.display.videoWhiteNits)};
    const auto width = static_cast<UINT>(client.right);
    const auto height = static_cast<UINT>(client.bottom);
    ScenePassRenderer scenes;
    NativeSettingsHost host;
    PresentationPass output;
    RequireSettingsHost(scenes.InitializeBorrowed(device.device.Get(), device.context.Get(),
        {width, height, whites, &prepared->data}), "Settings host scenes");
    RequireSettingsHost(output.Initialize(device.device.Get(), device.context.Get(),
        {width, height, static_cast<float>(loaded.value.display.displayPeakNits),
            &scenes, whites, host.Window()}), "Settings host output");
    NativeSettingsHostConfiguration configuration{device.device.Get(), context.Get(), &output,
        PresentationScope::qualifiedExclusive, window.Window(), width, height, 2,
        inputFile.Path(), loaded.value};
    FO4HDR_CHECK(!host.InitializeCold(configuration, prepared));
    FO4HDR_CHECK(prepared != nullptr);
    configuration.generation = 1;
    RequireSettingsHost(host.InitializeCold(configuration, prepared),
        "Settings host initial adoption");
    FO4HDR_CHECK(!prepared);
    auto& settings = *host.Window();
    (void)SendMessageW(window.Window(), WM_SETFOCUS, 0, 0);
    (void)SendMessageW(window.Window(), WM_KEYDOWN, VK_F10, toggleScan);
    (void)SendMessageW(window.Window(), WM_KEYUP, VK_F10, toggleScan | (1ULL << 31));
    FO4HDR_CHECK(forwardedToggles == 2);
    const auto overlay = settings.Draw(1, SettingsWindowScope::qualifiedExclusive);
    FO4HDR_CHECK(overlay.frame == 1);
    FO4HDR_CHECK(overlay.calibrationMode == 0);
    CheckOverlayPixels(device, overlay);
    FO4HDR_CHECK(settings.Draw(1, SettingsWindowScope::qualifiedExclusive).source == overlay.source);
    (void)SendMessageW(window.Window(), WM_KILLFOCUS, 0, 0);
    const auto closed = settings.Draw(2, SettingsWindowScope::qualifiedExclusive);
    FO4HDR_CHECK(closed.source == nullptr && closed.calibrationMode == 0);
    CheckSettingsHostResize(host, scenes, device, configuration);
    RequireSettingsHost(host.RetireCold(), "Settings host cold retirement");
    const auto retired = settings.Draw(4, SettingsWindowScope::qualifiedExclusive);
    FO4HDR_CHECK(retired.source == nullptr && retired.calibrationMode == 0);
    RequireSettingsHost(output.Retire(), "Settings host output retirement");
    RequireSettingsHost(scenes.Retire(), "Settings host scene retirement");
}

}  // namespace

void RunSettingsPanelTests()
{
    {
        const SettingsFrameContext context;
        LiveSettings live;
        for (const auto algorithm : {OperatorLutAlgorithm::aces2, OperatorLutAlgorithm::agX,
                 OperatorLutAlgorithm::gt7})
            RenderSettingsFrame(algorithm, live);
        // The caller dispatches panel intent through the real unavailable owner.
        DispatchPanelActions({true, true}, live, PluginConfiguration{});
        RenderCalibrationFrame();
    }
    RenderOwnedBackend();
}

}  // namespace fo4hdr::tests
