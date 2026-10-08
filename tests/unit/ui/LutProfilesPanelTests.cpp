// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "LutProfilesPanel.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace fo4hdr::tests {
namespace {

[[nodiscard]] ImageSpaceIdentity MakeImageSpace(
    std::string_view key, std::string_view editorId) noexcept
{
    ImageSpaceIdentity identity{};
    std::copy_n(key.data(), std::min(key.size(), identity.key.size() - 1), identity.key.data());
    std::copy_n(editorId.data(), std::min(editorId.size(), identity.editorId.size() - 1), identity.editorId.data());
    identity.formId = 0x00000111U;
    return identity;
}

void InitializePanelContext(float scale)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(1920.0F, 1400.0F);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    FO4HDR_CHECK(pixels != nullptr);
    FO4HDR_CHECK(width > 0 && height > 0);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
}

struct PanelFrame
{
    ImVec2 size;
    ImVec2& checkboxPosition;
    std::string* renderedText{};
};

[[nodiscard]] bool DrawPanelFrame(
    HdrSettings& settings, const WeatherLutResponseView& view, const PanelFrame& frame)
{
    const auto& [size, checkboxPosition, renderedText] = frame;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool changed{};
    if (ImGui::Begin("Weather response smoke", nullptr,
            ImGuiWindowFlags_NoSavedSettings))
    {
        checkboxPosition = ImGui::GetCursorScreenPos();
        checkboxPosition.x += ImGui::GetFrameHeight() * 0.5F;
        checkboxPosition.y += ImGui::GetFrameHeight() * 0.5F;
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Advanced Formula"), 1);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Current Contributors"), 1);
        const auto verticesBefore = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        if (renderedText != nullptr)
        {
            auto& platform = ImGui::GetPlatformIO();
            platform.Platform_ClipboardUserData = renderedText;
            platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
                auto* output = static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
                *output = text;
            };
            ImGui::LogToClipboard();
        }
        changed = DrawWeatherLutResponsePanel(settings, view);
        if (renderedText != nullptr) ImGui::LogFinish();
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > verticesBefore);
    }
    ImGui::End();
    ImGui::Render();
    const auto* drawData = ImGui::GetDrawData();
    FO4HDR_CHECK(drawData != nullptr);
    FO4HDR_CHECK(drawData->CmdListsCount > 0);
    FO4HDR_CHECK(drawData->TotalVtxCount > 0);
    FO4HDR_CHECK(drawData->TotalIdxCount > 0);
    return changed;
}

void RenderResponsePanel(
    ImVec2 windowSize, float scale, const WeatherLutResponseView& view,
    bool enabled)
{
    InitializePanelContext(scale);
    HdrSettings settings{};
    settings.weatherGradeResponse.enabled = enabled;
    settings.weatherGradeResponse.luminance.gain = 0.5;
    settings.weatherGradeResponse.color.minimum = 40.0;
    const auto before = settings;
    ImVec2 checkboxPosition;
    FO4HDR_CHECK(!DrawPanelFrame(settings, view, {windowSize, checkboxPosition}));
    FO4HDR_CHECK(settings == before);
    ImGui::DestroyContext();
}

void TestBypassedResponseStatus()
{
    InitializePanelContext(1.0F);
    HdrSettings settings{};
    WeatherLutResponseView view{};
    view.validatedSlots = 1U;
    view.imageSpaceValidSlots = 1U;
    view.automaticSlots = 1U;
    view.resolved[0] = {65.0, 72.0};
    view.gradingBypassed = true;
    view.fallbackReasons[0] = "Production constants unavailable.";
    ImVec2 checkboxPosition;
    std::string text;
    FO4HDR_CHECK(!DrawPanelFrame(settings, view, {ImVec2(900.0F, 1200.0F), checkboxPosition, &text}));
    FO4HDR_CHECK(text.contains("Production grading bypassed"));
    FO4HDR_CHECK(text.contains("Production constants unavailable."));
    FO4HDR_CHECK(!text.contains("Automatic response active."));
    FO4HDR_CHECK(!text.contains("Global fallback:"));
    FO4HDR_CHECK(!text.contains("L 65.0% / C 72.0%"));
    ImGui::DestroyContext();
}
void TestResponseEnableInput()
{
    InitializePanelContext(1.0F);
    HdrSettings settings{};
    const auto before = settings.weatherGradeResponse;
    const WeatherLutResponseView view{};
    const ImVec2 windowSize(900.0F, 1200.0F);
    ImVec2 checkboxPosition;
    FO4HDR_CHECK(!DrawPanelFrame(settings, view, {windowSize, checkboxPosition}));
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(checkboxPosition.x, checkboxPosition.y);
    FO4HDR_CHECK(!DrawPanelFrame(settings, view, {windowSize, checkboxPosition}));
    io.AddMouseButtonEvent(0, true);
    FO4HDR_CHECK(!DrawPanelFrame(settings, view, {windowSize, checkboxPosition}));
    io.AddMouseButtonEvent(0, false);
    FO4HDR_CHECK(DrawPanelFrame(settings, view, {windowSize, checkboxPosition}));
    auto expected = before;
    expected.enabled = !before.enabled;
    FO4HDR_CHECK(settings.weatherGradeResponse == expected);
    ImGui::DestroyContext();
}

}  // namespace

void RunLutProfilesPanelTests()
{
    WeatherLutResponseView waiting{};
    waiting.status = "No qualified LUT draw; literal %s stays text.";
    RenderResponsePanel(ImVec2(340.0F, 900.0F), 1.0F, waiting, true);
    RenderResponsePanel(ImVec2(340.0F, 900.0F), 1.0F, waiting, false);

    WeatherLutResponseView partial{};
    partial.status = "One native LUT identity is verified.";
    partial.imageSpaceStatus = "One ImageSpace aggregate is complete.";
    partial.currentImageSpaces[0] = MakeImageSpace("fallout4.esm|000111",
        "A very long editor ID with literal 100% and %s format text");
    partial.currentImageSpaces[1] = MakeImageSpace("fallout4.esm|000222", {});
    partial.currentImageSpaces[2].key.fill('x');
    partial.currentLuts[0].weight = 0.625F;
    partial.currentLuts[1].weight = 0.375F;
    partial.currentLuts[3].weight = std::numeric_limits<float>::infinity();
    partial.validatedSlots = 3U;
    partial.imageSpaceValidSlots = 3U;
    partial.automaticSlots = 1U;
    partial.referencingContexts = {24, 3, 0, 0};
    partial.resolved[0] = {65.0, 72.0};
    partial.resolved[1] = {67.0, 100.0};
    partial.resolved[3].colorPercent = std::numeric_limits<double>::quiet_NaN();
    partial.fallbackReasons[1] = "Incomplete weather data; global fallback at 100%.";
    RenderResponsePanel(ImVec2(340.0F, 900.0F), 1.0F, partial, true);
    RenderResponsePanel(ImVec2(900.0F, 1200.0F), 2.0F, partial, true);
    TestResponseEnableInput();
    TestBypassedResponseStatus();
}

}  // namespace fo4hdr::tests
