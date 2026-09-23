// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Fallout4HDR/config/HdrSettings.hpp"
#include <cstdint>
namespace fo4hdr {
struct DisplayCalibrationView
{
    const char* displayNameUtf8{};
    double reportedPeakNits{};
    double reportedFullFrameNits{};
    double osSdrWhiteNits{};
    bool identityAvailable{};
    bool profileBusy{};
    bool saveReady{};
    const char* status{};
};

struct DisplayCalibrationActions
{
    bool changed{};
    bool load{};
    bool save{};
    bool hideOverlay{};
};

[[nodiscard]] DisplayCalibrationActions DrawDisplayCalibrationPanel(HdrSettings& settings, std::uint32_t& mode, const DisplayCalibrationView& view) noexcept;
}
