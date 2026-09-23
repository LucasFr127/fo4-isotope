// SPDX-License-Identifier: GPL-3.0-or-later

#include "DisplayDiscovery.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <span>

namespace fo4hdr {
namespace {
using Microsoft::WRL::ComPtr;
HMONITOR g_activeMonitor{};
std::atomic<int> g_activeDisplayHdrState{-1};
DisplayCapabilities g_displayCapabilities;
ComPtr<IDXGIFactory1> g_displayFactory;
HMONITOR g_displayDiscoveryMonitor{};
ULONGLONG g_displayRetryAfter{};

[[nodiscard]] constexpr std::uint64_t DisplayIntersectionArea(
    RECT window, RECT display) noexcept
{
    const auto width = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(std::min(window.right, display.right)) -
        std::max(window.left, display.left));
    const auto height = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(std::min(window.bottom, display.bottom)) -
        std::max(window.top, display.top));
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
}
static_assert(DisplayIntersectionArea({-1920, 0, 100, 1080},
    {-1920, 0, 0, 1080}) == 2073600);
static_assert(DisplayIntersectionArea({-1920, 0, 100, 1080},
    {0, 0, 1920, 1080}) == 108000);
static_assert(DisplayIntersectionArea({0, 0, 100, 100},
    {100, 0, 200, 100}) == 0);
static_assert(DisplayIntersectionArea({-200, -200, -100, -100},
    {0, 0, 100, 100}) == 0);

const DISPLAYCONFIG_PATH_INFO* FindDisplayPath(std::span<const DISPLAYCONFIG_PATH_INFO> paths,
    const DisplayCapabilities& capabilities) noexcept
{
    const DISPLAYCONFIG_PATH_INFO* selected{};
    for (const auto& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
            sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName,
                capabilities.description.DeviceName) != 0)
            continue;
        if (selected != nullptr) return nullptr;
        selected = &path;
    }
    return selected;
}

void ReadDisplayTarget(const DISPLAYCONFIG_PATH_INFO& selected,
    DisplayCapabilities& capabilities) noexcept
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
        sizeof(target), selected.targetInfo.adapterId, selected.targetInfo.id};
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
    {
        std::copy_n(target.monitorDevicePath, capabilities.monitorDevicePath.size(),
            capabilities.monitorDevicePath.begin());
        std::copy_n(target.monitorFriendlyDeviceName, capabilities.friendlyName.size(),
            capabilities.friendlyName.begin());
        capabilities.monitorDevicePath.back() = L'\0';
        capabilities.friendlyName.back() = L'\0';
    }
}

void ReadDisplayWhite(const DISPLAYCONFIG_PATH_INFO& selected,
    DisplayCapabilities& capabilities) noexcept
{
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
        sizeof(white), selected.targetInfo.adapterId, selected.targetInfo.id};
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
        capabilities.sdrWhiteNits = static_cast<float>(white.SDRWhiteLevel) * 0.08F;
}

void ReadDisplayConfiguration(DisplayCapabilities& capabilities) noexcept
{
    // Fixed storage bounds discovery work; ambiguous clone paths have no profile key.
    std::array<DISPLAYCONFIG_PATH_INFO, 64> paths{};
    std::array<DISPLAYCONFIG_MODE_INFO, 128> modes{};
    UINT32 pathCount = static_cast<UINT32>(paths.size());
    UINT32 modeCount = static_cast<UINT32>(modes.size());
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
            &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return;
    const auto* selected = FindDisplayPath(std::span{paths.data(), pathCount}, capabilities);
    if (selected == nullptr) return;
    ReadDisplayTarget(*selected, capabilities);
    ReadDisplayWhite(*selected, capabilities);
}

struct DisplayOutputSearch
{
    RECT bounds{};
    DisplayCapabilities candidate{};
    std::uint64_t bestArea{};

    bool ConsiderOutput(const ComPtr<IDXGIOutput>& output) noexcept
    {
        DXGI_OUTPUT_DESC description{};
        if (FAILED(output->GetDesc(&description))) return false;
        if (!description.AttachedToDesktop) return true;
        const auto area = DisplayIntersectionArea(bounds, description.DesktopCoordinates);
        if (area <= bestArea) return true;
        bestArea = area;
        candidate = {};
        ComPtr<IDXGIOutput6> output6;
        candidate.known = SUCCEEDED(output.As(&output6)) && output6 &&
            SUCCEEDED(output6->GetDesc1(&candidate.description));
        return true;
    }

    bool ReadOutputs(IDXGIAdapter1* adapter) noexcept
    {
        for (UINT outputIndex = 0; outputIndex < 64; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            const auto result = adapter->EnumOutputs(outputIndex, &output);
            if (result == DXGI_ERROR_NOT_FOUND) return true;
            if (FAILED(result) || !output) return false;
            if (!ConsiderOutput(output)) return false;
        }
        return false;
    }

    bool ReadAdapters() noexcept
    {
        // The presenting device need not drive the window's monitor.
        for (UINT adapterIndex = 0; adapterIndex < 32; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const auto result = g_displayFactory->EnumAdapters1(adapterIndex, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) return true;
            if (FAILED(result) || !adapter) return false;
            if (!ReadOutputs(adapter.Get())) return false;
        }
        return false;
    }

    bool Publish() noexcept
    {
        if (!candidate.known || !g_displayFactory->IsCurrent()) return false;
        ReadDisplayConfiguration(candidate);
        if (!g_displayFactory->IsCurrent()) return false;
        g_displayCapabilities = candidate;
        g_displayRetryAfter = 0;
        return true;
    }
};

bool CachedDisplayMatches(HMONITOR monitor, bool factoryCurrent) noexcept
{
    return factoryCurrent && monitor == g_displayDiscoveryMonitor && g_displayCapabilities.known;
}

bool RefreshDisplayFactory(bool factoryCurrent) noexcept
{
    if (factoryCurrent) return true;
    g_displayFactory.Reset();
    return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&g_displayFactory)));
}

bool BeginDisplayDiscovery(HMONITOR monitor, bool factoryCurrent) noexcept
{
    g_displayCapabilities = {};
    const auto now = GetTickCount64();
    if (now < g_displayRetryAfter) return false;
    g_displayRetryAfter = now + 1000;
    g_displayDiscoveryMonitor = monitor;
    if (!RefreshDisplayFactory(factoryCurrent)) return false;
    return true;
}

}  // namespace

[[nodiscard]] bool RefreshDisplayCapabilities(HWND window) noexcept
{
    const auto monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONULL) : nullptr;
    const bool factoryCurrent = g_displayFactory && g_displayFactory->IsCurrent();
    if (CachedDisplayMatches(monitor, factoryCurrent)) return true;
    if (!BeginDisplayDiscovery(monitor, factoryCurrent)) return false;
    DisplayOutputSearch search;
    if (monitor == nullptr || !GetWindowRect(window, &search.bounds)) return false;
    if (!search.ReadAdapters()) return false;
    return search.Publish();
}
[[nodiscard]] bool ReferenceDisplayStillEligible() noexcept
{
    const auto observed = g_activeDisplayHdrState.load(
        std::memory_order_acquire);
    if (observed >= 0) return observed == 1;
    return g_displayCapabilities.known &&
        g_displayCapabilities.description.ColorSpace ==
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

const DisplayCapabilities& CurrentDisplayCapabilities() noexcept
{
    return g_displayCapabilities;
}

void InvalidateDisplayCapabilities() noexcept
{
    g_displayCapabilities = {};
}

void ResetDisplayDiscovery() noexcept
{
    g_activeMonitor = nullptr;
    g_displayFactory.Reset();
    g_displayCapabilities = {};
    g_displayDiscoveryMonitor = nullptr;
    g_displayRetryAfter = 0;
    g_activeDisplayHdrState.store(-1, std::memory_order_release);
}

HMONITOR ActiveDisplayMonitor() noexcept
{
    return g_activeMonitor;
}

int ActiveDisplayHdrState() noexcept
{
    return g_activeDisplayHdrState.load(std::memory_order_acquire);
}

void PublishActiveDisplay(HMONITOR monitor, int hdrState) noexcept
{
    g_activeMonitor = monitor;
    g_activeDisplayHdrState.store(hdrState, std::memory_order_release);
}

}  // namespace fo4hdr