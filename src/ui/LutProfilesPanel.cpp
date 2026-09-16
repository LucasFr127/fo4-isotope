// SPDX-License-Identifier: GPL-3.0-or-later

#include "LutProfilesPanel.hpp"
#include "HdrRenderer.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace fo4hdr {
namespace {

template<std::size_t Size>
std::string_view BoundedText(const std::array<char, Size>& text) noexcept
{
    const auto end = std::find(text.begin(), text.end(), '\0');
    if (end == text.end()) return {};
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

std::string_view LutPath(const LutSnapshot& lut) noexcept
{
    const auto length = static_cast<std::size_t>(lut.pathLength);
    if (length == 0 || length >= lut.utf8Path.size()) return {};
    if (lut.utf8Path[length] != '\0') return {};
    return {lut.utf8Path.data(), length};
}

std::string_view FileName(std::string_view path) noexcept
{
    const auto separator = path.find_last_of('/');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

bool IsValidWeight(const LutSnapshot& lut) noexcept
{
    const auto weight = lut.weight;
    return std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F;
}

const char* LutQualification(const LutSnapshot& lut, bool validated) noexcept
{
    if (LutPath(lut).empty()) return lut.weight == 0.0F ? "No active LUT" : "Unidentified LUT";
    return validated ? "Verified" : "Unverified";
}

const char* ImageSpaceQualification(const ImageSpaceIdentity& imageSpace, bool validated) noexcept
{
    if (BoundedText(imageSpace.key).empty()) return "Unavailable";
    return validated ? "Verified" : "Unverified";
}

bool HasOverride(const LutGradeProfile* profile, bool luminance) noexcept
{
    if (!profile) return false;
    return luminance ? profile->overrideLuminance : profile->overrideColor;
}

double& ComponentValue(LutGradeProfile* profile, bool luminance, double& fallback) noexcept
{
    if (!HasOverride(profile, luminance)) return fallback;
    return luminance ? profile->luminancePercent : profile->colorPercent;
}

bool NormalizeKey(GradeProfileKind kind, std::string_view key,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept
{
    if (kind == GradeProfileKind::lutAsset) return NormalizeLutGradePath(key, normalized);
    return kind == GradeProfileKind::imageSpace && NormalizeImageSpaceGradeKey(key, normalized);
}

void DrawStatus(const char* label, const char* status) noexcept
{
    ImGui::TextUnformatted(label);
    if (!status || status[0] == '\0') return;
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(status);
    ImGui::PopTextWrapPos();
}

void DrawIdentityTooltip(std::string_view identity, std::uint32_t formId) noexcept
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(identity.data(), identity.data() + identity.size());
    if (formId) ImGui::Text("Form ID: %08X", formId);
    ImGui::EndTooltip();
}

bool DrawProfileValue(const char* label, double& value, double inherited) noexcept
{
    constexpr double minimum = 0.0;
    constexpr double maximum = 100.0;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::SliderScalar("##value", ImGuiDataType_Double, &value,
        &minimum, &maximum, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    bool changed = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Ctrl+click to type an exact value. 100%% retains this grade response.");
    ImGui::BeginDisabled(value == inherited);
    if (ImGui::Button("Reset to inherited"))
    {
        value = inherited;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Restore the inherited value; this component's override stays enabled.");
    ImGui::PopID();
    return changed;
}

class ProfilePanel
{
public:
    ProfilePanel(HdrSettings& settings, const LutProfileDrawSnapshot& view,
        GradeProfileSelection& selection) noexcept : settings_(settings), view_(view), selection_(selection) {}

    bool Draw() noexcept
    {
        auto& profiles = settings_.lutGradeProfiles;
        bool changed = ImGui::Checkbox("Enable Grade Profiles", &profiles.enabled);
        ImGui::SetItemTooltip("Apply verified ImageSpace overrides over LUT overrides over global grade.\n"
            "Profiles remain editable while disabled; overall grading strength stays global.");
        DrawStatus("Current LUT qualification", view_.status);
        DrawStatus("Current ImageSpace qualification", view_.imageSpaceStatus);
        DrawCurrentInputs();
        changed |= DrawSelected();
        if (ImGui::CollapsingHeader("Saved Profiles")) DrawSaved();
        return changed;
    }

private:
    std::string_view SelectedKey() const noexcept { return BoundedText(selection_.key); }

    bool IsSelected(GradeProfileKind kind, std::string_view key) const noexcept
    {
        std::array<char, kMaximumLutGradePathLength + 1> normalized{};
        return selection_.kind == kind && NormalizeKey(kind, key, normalized) && selection_.key == normalized;
    }

    void Select(GradeProfileKind kind, std::string_view key) noexcept
    {
        std::array<char, kMaximumLutGradePathLength + 1> normalized{};
        if (NormalizeKey(kind, key, normalized)) selection_ = {kind, normalized};
    }

    LutGradeProfile* SelectedProfile() noexcept
    {
        auto& profiles = settings_.lutGradeProfiles;
        return selection_.kind == GradeProfileKind::lutAsset ?
            FindLutGradeProfile(profiles, SelectedKey()) : FindImageSpaceGradeProfile(profiles, SelectedKey());
    }

    bool SetSelected(const LutGradeOverride& values) noexcept
    {
        auto& profiles = settings_.lutGradeProfiles;
        return selection_.kind == GradeProfileKind::lutAsset ?
            SetLutGradeProfile(profiles, SelectedKey(), values) : SetImageSpaceGradeProfile(profiles, SelectedKey(), values);
    }

    bool RemoveSelected() noexcept
    {
        auto& profiles = settings_.lutGradeProfiles;
        return selection_.kind == GradeProfileKind::lutAsset ?
            RemoveLutGradeProfile(profiles, SelectedKey()) : RemoveImageSpaceGradeProfile(profiles, SelectedKey());
    }

    void DrawIdentity(GradeProfileKind kind, std::string_view key,
        std::string_view name, std::uint32_t formId = 0) noexcept
    {
        if (key.empty())
        {
            ImGui::TextDisabled("Unavailable");
            return;
        }
        ImGui::PushID(kind == GradeProfileKind::lutAsset ? "lut" : "image-space");
        if (ImGui::Selectable(name.data(), IsSelected(kind, key))) Select(kind, key);
        DrawIdentityTooltip(key, formId);
        ImGui::PopID();
    }

    void DrawCurrentRow(std::size_t index) noexcept
    {
        const auto& lut = view_.currentLuts[index];
        const auto lutPath = LutPath(lut);
        const auto& imageSpace = view_.currentImageSpaces[index];
        const auto imageKey = BoundedText(imageSpace.key);
        const auto editorId = BoundedText(imageSpace.editorId);
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Slot %zu", index + 1);
        DrawIdentity(GradeProfileKind::lutAsset, lutPath, FileName(lutPath));
        ImGui::TextDisabled("LUT: %s", LutQualification(lut, (view_.validatedSlots & (1U << index)) != 0));
        DrawIdentity(GradeProfileKind::imageSpace, imageKey, editorId.empty() ? imageKey : editorId, imageSpace.formId);
        ImGui::TextDisabled("ImageSpace: %s", ImageSpaceQualification(imageSpace, (view_.imageSpaceValidSlots & (1U << index)) != 0));
        ImGui::TableSetColumnIndex(1);
        if (IsValidWeight(lut))
            ImGui::Text("%.1f%%", static_cast<double>(lut.weight) * 100.0);
        else ImGui::TextDisabled("Invalid weight");
        ImGui::PopID();
    }

    void DrawCurrentInputs() noexcept
    {
        if (!ImGui::BeginTable("##current-grade-inputs", 2,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
        ImGui::TableSetupColumn("Current grade inputs");
        ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize("100.0%").x + ImGui::GetStyle().CellPadding.x * 2.0F);
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < view_.currentLuts.size(); ++index) DrawCurrentRow(index);
        ImGui::EndTable();
    }

    ResolvedLutGrade Inherited() const noexcept
    {
        const ResolvedLutGrade global{settings_.vanillaGradeLuminancePercent, settings_.vanillaGradeColorPercent};
        if (selection_.kind == GradeProfileKind::lutAsset)
        {
            ImGui::TextDisabled("Inherited values: global grade");
            return global;
        }
        for (std::size_t index = 0; index < view_.currentImageSpaces.size(); ++index)
        {
            if (!IsSelected(GradeProfileKind::imageSpace, BoundedText(view_.currentImageSpaces[index].key))) continue;
            const auto path = LutPath(view_.currentLuts[index]);
            if (path.empty()) continue;
            ImGui::Text("Inherits from active LUT: %s", FileName(path).data());
            return ResolveLutGradeProfile(settings_.lutGradeProfiles, path, global.luminancePercent, global.colorPercent);
        }
        ImGui::TextWrapped("ImageSpace inactive or LUT unidentified: showing global fallback values.");
        return global;
    }

    bool PersistFlags(bool luma, bool color, ResolvedLutGrade inherited) noexcept
    {
        if (!luma && !color) return RemoveSelected();
        const auto* profile = SelectedProfile();
        const auto luminance = profile && profile->overrideLuminance ? profile->luminancePercent : inherited.luminancePercent;
        const auto chromaticity = profile && profile->overrideColor ? profile->colorPercent : inherited.colorPercent;
        return SetSelected({luminance, luma, chromaticity, color});
    }

    bool DrawComponent(bool luma, ResolvedLutGrade inherited) noexcept
    {
        auto* profile = SelectedProfile();
        bool overrideLuma = HasOverride(profile, true);
        bool overrideColor = HasOverride(profile, false);
        auto& enabled = luma ? overrideLuma : overrideColor;
        bool changed{};
        if (ImGui::Checkbox(luma ? "Override Luminance" : "Override Color", &enabled))
            changed = PersistFlags(overrideLuma, overrideColor, inherited);
        ImGui::SetItemTooltip("Disable this component to inherit; the other component remains independent.");
        profile = SelectedProfile();
        enabled = HasOverride(profile, luma);
        double fallback = luma ? inherited.luminancePercent : inherited.colorPercent;
        auto& value = ComponentValue(profile, luma, fallback);
        ImGui::BeginDisabled(!enabled);
        changed |= DrawProfileValue(luma ? "Profile Luminance" : "Profile Color", value, fallback);
        ImGui::EndDisabled();
        return changed;
    }

    bool DrawEditor(ResolvedLutGrade inherited) noexcept
    {
        const auto& profiles = settings_.lutGradeProfiles;
        const bool full = !SelectedProfile() && profiles.count >= profiles.entries.size();
        if (full) ImGui::TextDisabled("The 64-profile limit has been reached.");
        ImGui::BeginDisabled(full);
        bool changed = DrawComponent(true, inherited);
        changed |= DrawComponent(false, inherited);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!SelectedProfile());
        if (ImGui::Button("Remove Profile")) changed |= RemoveSelected();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Remove both overrides and use inherited grade values.");
        return changed;
    }

    bool DrawSelected() noexcept
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Selected Profile");
        const auto key = SelectedKey();
        if (key.empty())
        {
            ImGui::TextDisabled("Select an active or saved profile to edit.");
            return false;
        }
        ImGui::TextDisabled("%s", selection_.kind == GradeProfileKind::lutAsset ? "LUT asset" : "ImageSpace");
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(key.data(), key.data() + key.size());
        ImGui::PopTextWrapPos();
        return DrawEditor(Inherited());
    }

    void DrawSavedRow(const LutGradeProfile& profile, std::size_t index) noexcept
    {
        const auto key = BoundedText(profile.path);
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", profile.kind == GradeProfileKind::lutAsset ? "LUT asset" : "ImageSpace");
        DrawIdentity(profile.kind, key, profile.kind == GradeProfileKind::lutAsset ? FileName(key) : key);
        ImGui::PopID();
    }

    void DrawSaved() noexcept
    {
        const auto& profiles = settings_.lutGradeProfiles;
        if (!profiles.count)
        {
            ImGui::TextDisabled("No saved grade profiles.");
            return;
        }
        if (!ImGui::BeginTable("##saved-grade-profiles", 1, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) return;
        const auto count = std::min(profiles.count, profiles.entries.size());
        for (std::size_t index = 0; index < count; ++index) DrawSavedRow(profiles.entries[index], index);
        ImGui::EndTable();
    }

    HdrSettings& settings_;
    const LutProfileDrawSnapshot& view_;
    GradeProfileSelection& selection_;
};

}  // namespace

bool DrawLutProfilesPanel(HdrSettings& settings, const LutProfileDrawSnapshot& view,
    GradeProfileSelection& selection) noexcept
{
    return ProfilePanel(settings, view, selection).Draw();
}

}  // namespace fo4hdr
