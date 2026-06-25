// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi1_6.h>
#include <array>

namespace fo4hdr {

struct DisplayCapabilities
{
    bool known{};
    DXGI_OUTPUT_DESC1 description{};
    std::array<wchar_t, 128> monitorDevicePath{};
    std::array<wchar_t, 64> friendlyName{};
    float sdrWhiteNits{};
};

// Cold discovery returns copied SDK facts; an unavailable identity has an empty path.
[[nodiscard]] DisplayCapabilities QueryDisplayCapabilities(HWND window) noexcept;

}  // namespace fo4hdr
