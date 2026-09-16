// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <array>

namespace fo4hdr {

struct LutProfileDrawSnapshot;

struct GradeProfileSelection
{
    GradeProfileKind kind{GradeProfileKind::lutAsset};
    std::array<char, kMaximumLutGradePathLength + 1> key{};
};

// The window owns selection independently of changing native slot order.
// Return value joins the host's existing request-before-save dispatch.
[[nodiscard]] bool DrawLutProfilesPanel(HdrSettings& settings,
    const LutProfileDrawSnapshot& view, GradeProfileSelection& selection) noexcept;

}  // namespace fo4hdr
