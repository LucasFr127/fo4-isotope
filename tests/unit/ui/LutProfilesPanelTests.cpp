// SPDX-License-Identifier: GPL-3.0-or-later

#include "UnitTestSuite.hpp"
#include "LutProfilesPanel.hpp"
#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace fo4hdr::tests {
namespace {

LutSnapshot MakeLut(std::string_view path, float weight, std::uint64_t identity) noexcept
{
    LutSnapshot result{};
    const auto length = std::min(path.size(), result.utf8Path.size() - 1);
    std::copy_n(path.data(), length, result.utf8Path.data());
    result.pathLength = static_cast<std::uint16_t>(length);
    result.weight = weight;
    result.engineTextureId = identity;
    return result;
}

ImageSpaceIdentity MakeImageSpace(std::string_view key,
    std::string_view editorId, std::uint32_t formId) noexcept
{
    ImageSpaceIdentity result{};
    const auto keyLength = std::min(key.size(), result.key.size() - 1);
    const auto nameLength = std::min(editorId.size(), result.editorId.size() - 1);
    std::copy_n(key.data(), keyLength, result.key.data());
    std::copy_n(editorId.data(), nameLength, result.editorId.data());
    result.formId = formId;
    return result;
}

class PanelContext
{
public:
    PanelContext() : previous_(ImGui::GetCurrentContext())
    {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        if (context_) ImGui::SetCurrentContext(context_);
    }

    ~PanelContext()
    {
        if (context_) ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous_);
    }

    bool Prepare(float scale)
    {
        FO4HDR_CHECK(context_ != nullptr);
        if (!context_) return false;
        auto& io = ImGui::GetIO();
        io.IniFilename = io.LogFilename = nullptr;
        io.DisplaySize = {1920.0F, 1400.0F};
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        FO4HDR_CHECK(pixels != nullptr);
        FO4HDR_CHECK(width > 0 && height > 0);
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;
        return pixels && width > 0 && height > 0;
    }

private:
    ImGuiContext* previous_;
    ImGuiContext* context_{};
};

struct PanelCase
{
    ImVec2 windowSize;
    float scale;
    const LutProfilesView& observation;
    GradeProfileKind selectedKind;
    std::string_view selectedKey;
    bool enabled{true};
};

GradeProfileSelection SelectionFor(const PanelCase& test)
{
    GradeProfileSelection result;
    result.kind = test.selectedKind;
    const auto normalized = result.kind == GradeProfileKind::lutAsset ?
        NormalizeLutGradePath(test.selectedKey, result.key) :
        NormalizeImageSpaceGradeKey(test.selectedKey, result.key);
    FO4HDR_CHECK(normalized);
    return result;
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

void RenderLutProfilesPanel(const PanelCase& test)
{
    PanelContext context;
    if (!context.Prepare(test.scale)) return;
    PluginConfiguration editable;
    auto& settings = editable;
    FO4HDR_CHECK(SetLutGradeProfile(settings.lutGradeProfiles,
        "textures/luts/saved-only.dds", {72.0, true, 100.0, false}));
    FO4HDR_CHECK(SetImageSpaceGradeProfile(settings.lutGradeProfiles,
        "fallout4.esm|012345", {100.0, false, 64.0, true}));
    settings.lutGradeProfiles.enabled = test.enabled;
    const auto before = editable;
    auto selection = SelectionFor(test);
    const auto selectionBefore = selection;

    ImGui::NewFrame();
    ImGui::SetNextWindowSize(test.windowSize, ImGuiCond_Always);
    if (ImGui::Begin("LUT profiles smoke", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        FO4HDR_CHECK(!DrawLutProfilesPanel(settings, test.observation, selection));
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > vertices);
    }
    ImGui::End();
    ImGui::Render();
    CheckDrawData();
    FO4HDR_CHECK(editable == before);
    FO4HDR_CHECK(selection.kind == selectionBefore.kind);
    FO4HDR_CHECK(selection.key == selectionBefore.key);
}

}  // namespace

void RunLutProfilesPanelTests()
{
    constexpr std::string_view longPercentPath{
        "textures/effects/luts/"
        "a-very-long-active-grade-name-that-keeps-going-through-a-narrow-"
        "panel-and-contains-literal-100%-and-%s-format-text.dds"};
    LutProfilesView empty;
    empty.status = "No qualified LUT draw; literal %s stays text.";
    empty.imageSpaceStatus = nullptr;
    auto waiting = empty;
    waiting.imageSpaceStatus = "ImageSpace observation is waiting.";
    LutProfilesView active;
    active.currentLuts = {MakeLut(longPercentPath, 0.625F, 11),
        MakeLut(longPercentPath, 0.375F, 22), LutSnapshot{}, LutSnapshot{}};
    active.currentImageSpaces = {
        MakeImageSpace("fallout4.esm|000111", "WarmInterior", 0x00000111U),
        MakeImageSpace("fallout4.esm|000222", {}, 0x00000222U),
        ImageSpaceIdentity{}, ImageSpaceIdentity{}};
    active.validatedSlots = active.imageSpaceValidSlots = 1;
    active.status = "One authored LUT identity is qualified.";
    active.imageSpaceStatus = "One authored ImageSpace identity is qualified.";
    auto unresolved = active;
    unresolved.currentLuts[2] = MakeLut({}, 0.125F, 33);
    unresolved.currentLuts[3] = MakeLut("textures/effects/luts/invalid-weight.dds",
        std::numeric_limits<float>::infinity(), 44);
    unresolved.validatedSlots = unresolved.imageSpaceValidSlots = 0;
    unresolved.status = "LUT identities are not verified.";
    unresolved.imageSpaceStatus = "ImageSpaces are not qualified.";
    auto scaled = active;
    scaled.validatedSlots = scaled.imageSpaceValidSlots = 3;
    scaled.status = "Two authored LUT rows; literal 100% stays text.";

    RenderLutProfilesPanel({{340.0F, 900.0F}, 1.0F, empty,
        GradeProfileKind::lutAsset, "textures/luts/saved-only.dds", false});
    RenderLutProfilesPanel({{340.0F, 900.0F}, 1.0F, waiting,
        GradeProfileKind::imageSpace, "fallout4.esm|012345"});
    RenderLutProfilesPanel({{340.0F, 900.0F}, 1.0F, unresolved,
        GradeProfileKind::lutAsset, longPercentPath});
    RenderLutProfilesPanel({{340.0F, 900.0F}, 1.0F, active,
        GradeProfileKind::imageSpace, "fallout4.esm|000111"});
    RenderLutProfilesPanel({{900.0F, 1200.0F}, 2.0F, scaled,
        GradeProfileKind::imageSpace, "fallout4.esm|000222"});
}

}  // namespace fo4hdr::tests
