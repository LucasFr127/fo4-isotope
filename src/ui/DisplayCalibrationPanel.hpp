// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <cstdint>

namespace fo4hdr {

struct DisplayCalibrationActions
{
    bool changed{};
    bool hideOverlay{};
};

[[nodiscard]] DisplayCalibrationActions DrawDisplayCalibrationPanel(
    HdrSettings& settings, std::uint32_t& mode) noexcept;

}  // namespace fo4hdr
