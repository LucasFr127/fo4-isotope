// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

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

[[nodiscard]] bool RefreshDisplayCapabilities(HWND window) noexcept;
[[nodiscard]] bool ReferenceDisplayStillEligible() noexcept;
[[nodiscard]] const DisplayCapabilities& CurrentDisplayCapabilities() noexcept;
void InvalidateDisplayCapabilities() noexcept;
void ResetDisplayDiscovery() noexcept;
[[nodiscard]] HMONITOR ActiveDisplayMonitor() noexcept;
[[nodiscard]] int ActiveDisplayHdrState() noexcept;
void PublishActiveDisplay(HMONITOR monitor, int hdrState) noexcept;

}  // namespace fo4hdr