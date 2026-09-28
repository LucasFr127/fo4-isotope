// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "LutProfilesPanel.hpp"
#include "TestHarness.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace fo4hdr::tests {
namespace {

[[nodiscard]] LutSnapshot MakeLut(
    std::string_view path, float weight, std::uint64_t identity) noexcept
{
    LutSnapshot lut{};
    const auto length = std::min(path.size(), lut.utf8Path.size() - 1);
    std::copy_n(path.data(), length, lut.utf8Path.data());
    lut.pathLength = static_cast<std::uint16_t>(length);
    lut.weight = weight;
    lut.engineTextureId = identity;
    return lut;
}

[[nodiscard]] ImageSpaceIdentity MakeImageSpace(
    std::string_view key,
    std::string_view editorId,
    std::uint32_t formId) noexcept
{
    ImageSpaceIdentity identity{};
    const auto keyLength = std::min(key.size(), identity.key.size() - 1);
    const auto editorIdLength =
        std::min(editorId.size(), identity.editorId.size() - 1);
    std::copy_n(key.data(), keyLength, identity.key.data());
    std::copy_n(editorId.data(), editorIdLength, identity.editorId.data());
    identity.formId = formId;
    return identity;
}

struct PanelCase
{
    ImVec2 windowSize;
    float scale;
    const std::array<LutSnapshot, 4>& currentLuts;
    std::uint32_t validatedSlots;
    const char* status;
    const std::array<ImageSpaceIdentity, 4>& currentImageSpaces;
    std::uint32_t imageSpaceValidSlots;
    const char* imageSpaceStatus;
    GradeProfileKind selectedKind;
    std::string_view selectedKey;
};

void RenderLutProfilesPanel(const PanelCase& testCase)
{
    const auto& [windowSize, scale, currentLuts, validatedSlots, status,
        currentImageSpaces, imageSpaceValidSlots, imageSpaceStatus,
        selectedKind, selectedKey] = testCase;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0F, 1400.0F);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    FO4HDR_CHECK(pixels != nullptr);
    FO4HDR_CHECK(width > 0 && height > 0);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;

    HdrSettings settings{};
    FO4HDR_CHECK(SetLutGradeProfile(settings.lutGradeProfiles,
        "textures/luts/saved-only.dds", { 72.0, true, 100.0, false}));
    FO4HDR_CHECK(SetImageSpaceGradeProfile(settings.lutGradeProfiles,
        "fallout4.esm|012345", { 100.0, false, 64.0, true}));
    const auto profilesBefore = settings.lutGradeProfiles;
    fo4hdr::detail::SelectGradeProfile(selectedKind, selectedKey);
    std::array<char, kMaximumLutGradePathLength + 1> expectedSelection{};
    FO4HDR_CHECK(fo4hdr::detail::NormalizeGradeProfileKey(
        selectedKind, selectedKey, expectedSelection));
    FO4HDR_CHECK(fo4hdr::detail::SelectedGradeProfile().kind == selectedKind);
    FO4HDR_CHECK(
        fo4hdr::detail::SelectedGradeProfile().key == expectedSelection);

    ImGui::NewFrame();
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    if (ImGui::Begin("LUT profiles smoke", nullptr,
            ImGuiWindowFlags_NoSavedSettings))
    {
        const auto verticesBefore =
            ImGui::GetWindowDrawList()->VtxBuffer.Size;
        FO4HDR_CHECK(!DrawLutProfilesPanel(
            settings, {currentLuts, validatedSlots, status,
            currentImageSpaces, imageSpaceValidSlots, imageSpaceStatus}));
        FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size >
            verticesBefore);
    }
    ImGui::End();
    ImGui::Render();

    const auto* drawData = ImGui::GetDrawData();
    FO4HDR_CHECK(drawData != nullptr);
    FO4HDR_CHECK(drawData->CmdListsCount > 0);
    FO4HDR_CHECK(drawData->TotalVtxCount > 0);
    FO4HDR_CHECK(drawData->TotalIdxCount > 0);
    FO4HDR_CHECK(settings.lutGradeProfiles == profilesBefore);
    ImGui::DestroyContext();
}

}  // namespace

void RunLutProfilesPanelTests()
{
    constexpr std::string_view longPercentPath{
        "textures/effects/luts/"
        "a-very-long-active-grade-name-that-keeps-going-through-a-narrow-"
        "panel-and-contains-literal-100%-and-%s-format-text.dds"};
    const std::array<LutSnapshot, 4> empty{};
    const std::array<LutSnapshot, 4> active{
        MakeLut(longPercentPath, 0.625F, 11),
        MakeLut(longPercentPath, 0.375F, 22),
        LutSnapshot{},
        LutSnapshot{}};
    const std::array<LutSnapshot, 4> unresolved{
        active[0],
        active[1],
        MakeLut({}, 0.125F, 33),
        MakeLut("textures/effects/luts/invalid-weight.dds",
            std::numeric_limits<float>::infinity(), 44)};
    const std::array<ImageSpaceIdentity, 4> noImageSpaces{};
    const std::array<ImageSpaceIdentity, 4> activeImageSpaces{
        MakeImageSpace("fallout4.esm|000111", "WarmInterior", 0x00000111U),
        MakeImageSpace("fallout4.esm|000222", {}, 0x00000222U),
        ImageSpaceIdentity{},
        ImageSpaceIdentity{}};

    RenderLutProfilesPanel({ImVec2(340.0F, 900.0F), 1.0F,
        empty, 0U, "No qualified LUT draw; literal %s stays text.",
        noImageSpaces, 0U, nullptr, GradeProfileKind::lutAsset,
        "textures/luts/saved-only.dds"});
    RenderLutProfilesPanel({ImVec2(340.0F, 900.0F), 1.0F,
        empty, 0U, "No current LUT draw.", noImageSpaces, 0U,
        "ImageSpace observation is waiting.", GradeProfileKind::imageSpace,
        "fallout4.esm|012345"});
    RenderLutProfilesPanel({ImVec2(340.0F, 900.0F), 1.0F,
        unresolved, 0U, "LUT identities are not verified.",
        activeImageSpaces, 0U, "ImageSpaces are not qualified.",
        GradeProfileKind::lutAsset, longPercentPath});
    RenderLutProfilesPanel({ImVec2(340.0F, 900.0F), 1.0F,
        active, 1U, "One LUT identity is verified.", activeImageSpaces, 1U,
        "One ImageSpace is qualified.", GradeProfileKind::imageSpace,
        "fallout4.esm|000111"});
    RenderLutProfilesPanel({ImVec2(900.0F, 1200.0F), 2.0F,
        active, 15U, "LUT identities verified at 100% confidence.",
        activeImageSpaces, 3U, "ImageSpaces qualified for this draw.",
        GradeProfileKind::imageSpace, "fallout4.esm|000222"});
}

}  // namespace fo4hdr::tests
