// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include <array>
#include <cstdint>
#include <string_view>
namespace fo4hdr {
[[nodiscard]] bool DrawLutProfilesPanel(HdrSettings& settings, const LutProfilesView& view) noexcept;
namespace detail {
struct GradeProfileSelection
{
    GradeProfileKind kind{GradeProfileKind::lutAsset};
    std::array<char, kMaximumLutGradePathLength + 1> key{};
};

[[nodiscard]] GradeProfileSelection& SelectedGradeProfile() noexcept;
[[nodiscard]] bool NormalizeGradeProfileKey(GradeProfileKind kind, std::string_view key, std::array<char, kMaximumLutGradePathLength + 1>& normalized) noexcept;
void SelectGradeProfile(GradeProfileKind kind, std::string_view key) noexcept;
}
}
