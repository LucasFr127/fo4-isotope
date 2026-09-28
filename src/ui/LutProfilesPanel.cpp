// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "LutProfilesPanel.hpp"

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/NativeLutObservation.hpp"

#include "SettingsPanelSlider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace fo4hdr {

namespace detail {

template <std::size_t Size>
[[nodiscard]] std::string_view BoundedText(
    const std::array<char, Size>& text) noexcept
{
    const auto end = std::find(text.begin(), text.end(), '\0');
    if (end == text.end()) return {};
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

[[nodiscard]] std::string_view LutSnapshotPath(
    const LutSnapshot& lut) noexcept
{
    const auto length = static_cast<std::size_t>(lut.pathLength);
    if (length == 0 || length >= lut.utf8Path.size()) return {};
    if (lut.utf8Path[length] != '\0') return {};
    return {lut.utf8Path.data(), length};
}

[[nodiscard]] std::string_view LutProfilePath(
    const LutGradeProfile& profile) noexcept
{
    return BoundedText(profile.path);
}

[[nodiscard]] std::string_view LutFileName(
    std::string_view path) noexcept
{
    const auto separator = path.find_last_of('/');
    return separator == std::string_view::npos ? path :
        path.substr(separator + 1);
}

[[nodiscard]] GradeProfileSelection& SelectedGradeProfile() noexcept
{
    // Slot order and weights may change without retargeting the open editor.
    static GradeProfileSelection selected{};
    return selected;
}

[[nodiscard]] bool NormalizeGradeProfileKey(
    GradeProfileKind kind,
    std::string_view key,
    std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept
{
    return kind == GradeProfileKind::lutAsset ?
        NormalizeLutGradePath(key, normalized) :
        kind == GradeProfileKind::imageSpace &&
            NormalizeImageSpaceGradeKey(key, normalized);
}

[[nodiscard]] bool IsSelectedGradeProfile(
    GradeProfileKind kind, std::string_view key) noexcept
{
    std::array<char, kMaximumLutGradePathLength + 1> normalized{};
    const auto& selected = SelectedGradeProfile();
    return selected.kind == kind &&
        NormalizeGradeProfileKey(kind, key, normalized) &&
        selected.key == normalized;
}

void SelectGradeProfile(
    GradeProfileKind kind, std::string_view key) noexcept
{
    std::array<char, kMaximumLutGradePathLength + 1> normalized{};
    if (NormalizeGradeProfileKey(kind, key, normalized))
        SelectedGradeProfile() = {kind, normalized};
}

void SelectLutProfilePath(std::string_view path) noexcept
{
    SelectGradeProfile(GradeProfileKind::lutAsset, path);
}

void SelectImageSpaceProfileKey(std::string_view key) noexcept
{
    SelectGradeProfile(GradeProfileKind::imageSpace, key);
}

[[nodiscard]] std::string_view SelectedGradeProfileKey() noexcept
{
    return BoundedText(SelectedGradeProfile().key);
}

void DrawIdentityTooltip(
    std::string_view identity, std::uint32_t formId = 0) noexcept
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(
        identity.data(), identity.data() + identity.size());
    if (formId != 0) ImGui::Text("Form ID: %08X", formId);
    ImGui::EndTooltip();
}

[[nodiscard]] bool IsValidLutWeight(float weight) noexcept
{
    return std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F;
}

[[nodiscard]] const char* LutSlotValidationLabel(
    std::string_view path, float weight, bool identityValidated) noexcept
{
    if (path.empty())
        return weight == 0.0F ? "No active LUT" : "Unidentified LUT";
    return identityValidated ? "Verified" : "Unverified";
}

[[nodiscard]] const LutGradeProfile* FindSelectedGradeProfile(
    const LutGradeProfiles& profiles) noexcept
{
    return SelectedGradeProfile().kind == GradeProfileKind::lutAsset ?
        FindLutGradeProfile(profiles, SelectedGradeProfileKey()) :
        FindImageSpaceGradeProfile(profiles, SelectedGradeProfileKey());
}

[[nodiscard]] LutGradeProfile* FindSelectedGradeProfile(
    LutGradeProfiles& profiles) noexcept
{
    return const_cast<LutGradeProfile*>(FindSelectedGradeProfile(
        static_cast<const LutGradeProfiles&>(profiles)));
}

[[nodiscard]] bool SetSelectedGradeProfile(
    LutGradeProfiles& profiles,
    const LutGradeOverride& values) noexcept
{
    return SelectedGradeProfile().kind == GradeProfileKind::lutAsset ?
        SetLutGradeProfile(profiles, SelectedGradeProfileKey(), values) :
        SetImageSpaceGradeProfile(profiles, SelectedGradeProfileKey(), values);
}

[[nodiscard]] bool RemoveSelectedGradeProfile(
    LutGradeProfiles& profiles) noexcept
{
    return SelectedGradeProfile().kind == GradeProfileKind::lutAsset ?
        RemoveLutGradeProfile(profiles, SelectedGradeProfileKey()) :
        RemoveImageSpaceGradeProfile(profiles, SelectedGradeProfileKey());
}

}  // namespace detail

struct GradeValidationCount
{
    std::size_t present{};
    std::size_t verified{};

    void Add(bool isPresent, bool isVerified) noexcept
    {
        if (!isPresent) return;
        ++present;
        if (isVerified) ++verified;
    }

    const char* Label(const char* empty) const noexcept
    {
        if (present == 0) return empty;
        if (verified == present) return "Verified";
        if (verified != 0) return "Partial";
        return "Unverified";
    }
};

GradeValidationCount CountCurrentLuts(const LutProfilesView& view) noexcept
{
    GradeValidationCount count;
    for (std::size_t index = 0; index < view.currentLuts.size(); ++index)
    {
        const auto& lut = view.currentLuts[index];
        const auto path = detail::LutSnapshotPath(lut);
        count.Add(!path.empty() || lut.weight != 0.0F,
            !path.empty() && (view.validatedSlots & (1U << index)) != 0);
    }
    return count;
}

GradeValidationCount CountCurrentImageSpaces(const LutProfilesView& view) noexcept
{
    GradeValidationCount count;
    for (std::size_t index = 0; index < view.currentImageSpaces.size(); ++index)
        count.Add(!detail::BoundedText(view.currentImageSpaces[index].key).empty(),
            (view.imageSpaceValidSlots & (1U << index)) != 0);
    return count;
}

void DrawGradeStatus(const char* status) noexcept
{
    if (status == nullptr || status[0] == '\0') return;
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(status);
    ImGui::PopTextWrapPos();
}

void DrawLutProfileQualification(const LutProfilesView& view) noexcept
{
    const auto luts = CountCurrentLuts(view);
    const auto imageSpaces = CountCurrentImageSpaces(view);
    ImGui::Text("LUT validation: %s", luts.Label("Missing"));
    DrawGradeStatus(view.status);
    ImGui::Text("ImageSpace qualification: %s", imageSpaces.Label("Unavailable"));
    DrawGradeStatus(view.imageSpaceStatus);
}

void DrawCurrentLutIdentity(std::string_view lutPath) noexcept
{
    ImGui::TextDisabled("LUT");
    ImGui::SameLine();
    if (lutPath.empty())
    {
        ImGui::TextDisabled("No LUT identity");
    }
    else
    {
        ImGui::PushID("lut");
        const auto name = detail::LutFileName(lutPath);
        if (ImGui::Selectable(name.data(),
                detail::IsSelectedGradeProfile(
                    GradeProfileKind::lutAsset, lutPath)))
            detail::SelectLutProfilePath(lutPath);
        detail::DrawIdentityTooltip(lutPath);
        ImGui::PopID();
    }
}

void DrawCurrentImageSpaceIdentity(const ImageSpaceIdentity& imageSpace) noexcept
{
    const auto imageSpaceKey = detail::BoundedText(imageSpace.key);
    const auto editorId = detail::BoundedText(imageSpace.editorId);
    const auto imageSpaceName = editorId.empty() ? imageSpaceKey : editorId;
    ImGui::TextDisabled("ImageSpace");
    ImGui::SameLine();
    if (imageSpaceKey.empty())
    {
        ImGui::TextDisabled("Unavailable");
    }
    else
    {
        ImGui::PushID("image-space");
        if (ImGui::Selectable(imageSpaceName.data(),
                detail::IsSelectedGradeProfile(
                    GradeProfileKind::imageSpace, imageSpaceKey)))
            detail::SelectImageSpaceProfileKey(imageSpaceKey);
        detail::DrawIdentityTooltip(
            imageSpaceKey, imageSpace.formId);
        ImGui::PopID();
    }
}

void BeginCurrentGradeInputRow(std::size_t index, bool wide) noexcept
{
    ImGui::PushID(static_cast<int>(index));
    ImGui::TableNextRow();
    if (wide)
    {
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%zu", index + 1);
    }
    ImGui::TableSetColumnIndex(wide ? 1 : 0);
    if (!wide) ImGui::Text("Slot %zu", index + 1);

}

void DrawCurrentGradeInputRow(const LutProfilesView& view, std::size_t index, bool wide) noexcept
{
    const auto& [currentLuts, validatedSlots, status, currentImageSpaces,
        imageSpaceValidSlots, imageSpaceStatus] = view;
    const auto& lut = currentLuts[index];
    const auto lutPath = detail::LutSnapshotPath(lut);
    const auto& imageSpace = currentImageSpaces[index];
    const auto imageSpaceKey = detail::BoundedText(imageSpace.key);
    const auto* lutState = detail::LutSlotValidationLabel(lutPath,
        lut.weight, (validatedSlots & (1U << index)) != 0);
    const auto* imageSpaceState = imageSpaceKey.empty() ?
        "Unavailable" :
        (imageSpaceValidSlots & (1U << index)) != 0 ?
            "Verified" : "Unverified";

    BeginCurrentGradeInputRow(index, wide);
    DrawCurrentLutIdentity(lutPath);
    if (!wide) ImGui::TextDisabled("LUT: %s", lutState);

    DrawCurrentImageSpaceIdentity(imageSpace);
    if (!wide)
        ImGui::TextDisabled("ImageSpace: %s", imageSpaceState);

    ImGui::TableSetColumnIndex(wide ? 2 : 1);
    if (detail::IsValidLutWeight(lut.weight))
        ImGui::Text("%.1f%%",
            static_cast<double>(lut.weight) * 100.0);
    else
        ImGui::TextDisabled("Invalid");
    if (wide)
    {
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("LUT: %s", lutState);
        ImGui::Text("ImageSpace: %s", imageSpaceState);
    }
    ImGui::PopID();
}

void DrawCurrentGradeInputs(const LutProfilesView& view) noexcept
{
    const auto& [currentLuts, validatedSlots, status, currentImageSpaces,
        imageSpaceValidSlots, imageSpaceStatus] = view;
    const bool wide = ImGui::GetContentRegionAvail().x >= 520.0F;
    const auto columnCount = wide ? 4 : 2;
    if (ImGui::BeginTable("##current-grade-inputs", columnCount,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp))
    {
        if (wide)
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed,
                ImGui::CalcTextSize("Slot").x +
                    ImGui::GetStyle().CellPadding.x * 2.0F);
        ImGui::TableSetupColumn("Active grade inputs");
        ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize("100.0%").x +
                ImGui::GetStyle().CellPadding.x * 2.0F);
        if (wide)
            ImGui::TableSetupColumn("Qualification",
                ImGuiTableColumnFlags_WidthFixed,
                ImGui::CalcTextSize("ImageSpace: Unverified").x +
                    ImGui::GetStyle().CellPadding.x * 2.0F);
        ImGui::TableHeadersRow();

        for (std::size_t index = 0; index < currentLuts.size(); ++index)
        {
            DrawCurrentGradeInputRow(view, index, wide);
        }
        ImGui::EndTable();
    }
}

void DrawSavedGradeProfileRow(const LutGradeProfile& profile, std::size_t index, bool savedWide) noexcept
{
    const auto key = detail::LutProfilePath(profile);
    const auto* type = profile.kind == GradeProfileKind::lutAsset ?
        "LUT asset" : "ImageSpace";
    const auto name = profile.kind == GradeProfileKind::lutAsset ?
        detail::LutFileName(key) : key;
    ImGui::PushID(static_cast<int>(index));
    ImGui::TableNextRow();
    if (savedWide)
    {
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(type);
        ImGui::TableSetColumnIndex(1);
    }
    else
    {
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", type);
    }
    if (ImGui::Selectable(name.data(),
            detail::IsSelectedGradeProfile(profile.kind, key)))
        detail::SelectGradeProfile(profile.kind, key);
    detail::DrawIdentityTooltip(key);
    ImGui::PopID();
}

void DrawSavedGradeProfiles(const LutGradeProfiles& profiles) noexcept
{
    ImGui::Spacing();
    if (profiles.count == 0)
    {
        ImGui::TextDisabled("No saved grade profiles.");
    }
    else
    {
        const bool savedWide = ImGui::GetContentRegionAvail().x >= 420.0F;
        if (ImGui::BeginTable("##saved-grade-profiles", savedWide ? 2 : 1,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg))
        {
            if (savedWide)
            {
                ImGui::TableSetupColumn("Type",
                    ImGuiTableColumnFlags_WidthFixed,
                    ImGui::CalcTextSize("ImageSpace").x +
                        ImGui::GetStyle().CellPadding.x * 2.0F);
                ImGui::TableSetupColumn("Identity");
                ImGui::TableHeadersRow();
            }
            const auto count = std::min(
                profiles.count, profiles.entries.size());
            for (std::size_t index = 0; index < count; ++index)
            {
                DrawSavedGradeProfileRow(profiles.entries[index], index, savedWide);
            }
            ImGui::EndTable();
        }
    }
}

std::pair<bool, std::string_view> FindSelectedImageSpaceLut(const LutProfilesView& view) noexcept
{
    const auto& currentLuts = view.currentLuts;
    const auto& currentImageSpaces = view.currentImageSpaces;
    bool activeImageSpace{};
    std::string_view activeLutPath;
    for (std::size_t index = 0; index < currentImageSpaces.size(); ++index)
        if (detail::IsSelectedGradeProfile(GradeProfileKind::imageSpace,
                detail::BoundedText(currentImageSpaces[index].key)))
        {
            activeImageSpace = true;
            activeLutPath = detail::LutSnapshotPath(currentLuts[index]);
            if (!activeLutPath.empty()) break;
        }
    return {activeImageSpace, activeLutPath};
}

void DrawInheritedGrade(const HdrSettings& settings, const LutProfilesView& view,
    double& inheritedLuminance, double& inheritedColor) noexcept
{
    const auto& profiles = settings.lutGradeProfiles;
    const auto selectedKind = detail::SelectedGradeProfile().kind;
    if (selectedKind == GradeProfileKind::imageSpace)
    {
        const auto [activeImageSpace, activeLutPath] = FindSelectedImageSpaceLut(view);
        if (!activeImageSpace)
        {
            ImGui::TextWrapped("ImageSpace inactive: showing global fallback values.");
        }
        else if (activeLutPath.empty())
        {
            ImGui::TextWrapped("LUT unidentified: showing global fallback values.");
        }
        else
        {
            const auto inherited = ResolveLutGradeProfile(profiles,
                activeLutPath, settings.vanillaGradeLuminancePercent,
                settings.vanillaGradeColorPercent);
            inheritedLuminance = inherited.luminancePercent;
            inheritedColor = inherited.colorPercent;
            ImGui::Text("Inherits from active LUT: %s",
                detail::LutFileName(activeLutPath).data());
        }
    }
    else
    {
        ImGui::TextDisabled("Inherited values: global grade");
    }

}

class GradeProfileEditor
{
public:
    GradeProfileEditor(LutGradeProfiles& values, double inheritedLuminanceValue,
        double inheritedColorValue) noexcept : profiles(values),
        inheritedLuminance(inheritedLuminanceValue), inheritedColor(inheritedColorValue),
        profile(detail::FindSelectedGradeProfile(profiles)),
        overrideLuminance(profile != nullptr && profile->overrideLuminance),
        overrideColor(profile != nullptr && profile->overrideColor),
        luminance(overrideLuminance ? profile->luminancePercent : inheritedLuminance),
        color(overrideColor ? profile->colorPercent : inheritedColor)
    {}

    bool Draw() noexcept
    {
        const bool profileLimitReached = profile == nullptr &&
            profiles.count >= profiles.entries.size();
        if (profileLimitReached)
            ImGui::TextDisabled("The 64-profile limit has been reached.");
        ImGui::BeginDisabled(profileLimitReached);
        DrawLuminance();
        DrawColor();
        ImGui::EndDisabled();
        profile = detail::FindSelectedGradeProfile(profiles);
        ImGui::BeginDisabled(profile == nullptr);
        if (ImGui::Button("Remove Profile"))
            changed |= detail::RemoveSelectedGradeProfile(profiles);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Remove this profile and use inherited grade values.");
        return changed;
    }

private:
    bool Persist() noexcept
    {
        if (!overrideLuminance && !overrideColor)
            return detail::RemoveSelectedGradeProfile(profiles);
        return detail::SetSelectedGradeProfile(profiles, {luminance,
            overrideLuminance, color, overrideColor});
    }

    void DrawLuminance() noexcept
    {
        if (ImGui::Checkbox("Override Luminance", &overrideLuminance))
            changed |= Persist();
        ImGui::SetItemTooltip("Use this profile's luminance response instead of the inherited value.");
        profile = detail::FindSelectedGradeProfile(profiles);
        luminance = profile != nullptr && profile->overrideLuminance ?
            profile->luminancePercent : inheritedLuminance;
        ImGui::BeginDisabled(!overrideLuminance);
        changed |= detail::DrawSettingsPanelSlider(
            "Profile Luminance", profile != nullptr && overrideLuminance ?
                profile->luminancePercent : luminance, {
            inheritedLuminance, 0.0, 100.0, 1.0, "%.0f%%",
            "Adjusts this profile's grade luminance response. 0% removes the response; 100% keeps it.",
            "Restore the inherited luminance value; the override stays enabled."});
        ImGui::EndDisabled();
    }

    void DrawColor() noexcept
    {
        if (ImGui::Checkbox("Override Color", &overrideColor))
            changed |= Persist();
        ImGui::SetItemTooltip("Use this profile's color response instead of the inherited value.");
        profile = detail::FindSelectedGradeProfile(profiles);
        color = profile != nullptr && profile->overrideColor ?
            profile->colorPercent : inheritedColor;
        ImGui::BeginDisabled(!overrideColor);
        changed |= detail::DrawSettingsPanelSlider(
            "Profile Color", profile != nullptr && overrideColor ?
                profile->colorPercent : color, {
            inheritedColor, 0.0, 100.0, 1.0, "%.0f%%",
            "Adjusts this profile's grade hue and saturation response. "
            "0% retains the original scene colors; 100% keeps the full response.",
            "Restore the inherited color value; the override stays enabled."});
        ImGui::EndDisabled();
    }

    LutGradeProfiles& profiles;
    double inheritedLuminance;
    double inheritedColor;
    LutGradeProfile* profile;
    bool overrideLuminance;
    bool overrideColor;
    double luminance;
    double color;
    bool changed{};
};

bool DrawSelectedGradeProfile(HdrSettings& settings, const LutProfilesView& view) noexcept
{
    auto& profiles = settings.lutGradeProfiles;
    bool changed{};
    ImGui::Spacing();
    ImGui::SeparatorText("Selected Profile");
    const auto selectedKey = detail::SelectedGradeProfileKey();
    if (selectedKey.empty())
    {
        ImGui::TextDisabled("Select an active or saved profile to edit.");
        return changed;
    }

    const auto selectedKind = detail::SelectedGradeProfile().kind;
    ImGui::TextDisabled("%s", selectedKind == GradeProfileKind::lutAsset ?
        "LUT asset" : "ImageSpace");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(
        selectedKey.data(), selectedKey.data() + selectedKey.size());
    ImGui::PopTextWrapPos();

    double inheritedLuminance = settings.vanillaGradeLuminancePercent;
    double inheritedColor = settings.vanillaGradeColorPercent;
    DrawInheritedGrade(settings, view, inheritedLuminance, inheritedColor);

    return GradeProfileEditor(profiles, inheritedLuminance, inheritedColor).Draw();
}

[[nodiscard]] bool DrawLutProfilesPanel(
    HdrSettings& settings,
    const LutProfilesView& view) noexcept
{
    auto& profiles = settings.lutGradeProfiles;
    bool changed{};
    if (ImGui::Checkbox("Enable Grade Profiles", &profiles.enabled))
        changed = true;
    ImGui::SetItemTooltip("Apply profile overrides: verified ImageSpace first, then LUT, then global grade.\n"
        "Profiles remain editable while disabled; overall grading strength stays global.");

    DrawLutProfileQualification(view);
    DrawCurrentGradeInputs(view);

    changed |= DrawSelectedGradeProfile(settings, view);
    if (ImGui::CollapsingHeader("Saved Profiles"))
        DrawSavedGradeProfiles(profiles);
    return changed;

}

}  // namespace fo4hdr
