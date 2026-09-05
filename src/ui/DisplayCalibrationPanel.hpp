// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"

#include <cstdint>

namespace fo4hdr {

struct DisplayCalibrationView
{
    bool known{};
    bool identityAvailable{};
    const wchar_t* name{};
    double reportedPeakNits{};
    double reportedFullFrameNits{};
    double sdrWhiteNits{};
    bool profileAvailable{};
    SettingsPersistenceState profileState{SettingsPersistenceState::unavailable};
};

struct DisplayCalibrationActions
{
    bool changed{};
    bool hideOverlay{};
    bool refreshDisplay{};
    bool saveProfile{};
    bool loadProfile{};
};

[[nodiscard]] DisplayCalibrationActions DrawDisplayCalibrationPanel(
    HdrSettings& settings, std::uint32_t& mode, DisplayCalibrationView display = {}) noexcept;

}  // namespace fo4hdr
