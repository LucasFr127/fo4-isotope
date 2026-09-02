// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <thread>

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

// The cache is confined to its creation thread and invalidates copied facts on monitor/factory changes.
class DisplayDiscovery
{
public:
    DisplayDiscovery() = default;
    DisplayDiscovery(const DisplayDiscovery&) = delete;
    DisplayDiscovery& operator=(const DisplayDiscovery&) = delete;
    [[nodiscard]] DisplayCapabilities Refresh(HWND window) noexcept;
    void Reset() noexcept;

private:
    bool SelectMonitor(HMONITOR monitor) noexcept;
    bool MatchesMonitor(HMONITOR monitor) const noexcept;
    bool BeginDiscovery() noexcept;
    void Discover(HWND window) noexcept;

    std::thread::id owner_{std::this_thread::get_id()};
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    DisplayCapabilities capabilities_;
    HMONITOR monitor_{};
    ULONGLONG lastAttempt_{};
    bool attempted_{};
};

}  // namespace fo4hdr
