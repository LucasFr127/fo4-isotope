// SPDX-License-Identifier: GPL-3.0-or-later

#include "DisplayDiscovery.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <span>

namespace fo4hdr {
namespace {
using Microsoft::WRL::ComPtr;

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
    ComPtr<IDXGIFactory1> factory;
    RECT bounds{};
    DisplayCapabilities candidate{};
    std::uint64_t bestArea{};
    bool ambiguous{};

    bool ReadWindow(HWND window) noexcept
    {
        if (window == nullptr || !GetWindowRect(window, &bounds)) return false;
        return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    }

    bool SelectArea(std::uint64_t area) noexcept
    {
        if (area == 0 || area < bestArea) return false;
        if (area == bestArea)
        {
            ambiguous = true;
            return false;
        }
        bestArea = area;
        ambiguous = false;
        return true;
    }

    bool ConsiderOutput(const ComPtr<IDXGIOutput>& output) noexcept
    {
        DXGI_OUTPUT_DESC description{};
        if (FAILED(output->GetDesc(&description))) return false;
        if (!description.AttachedToDesktop) return true;
        const auto area = DisplayIntersectionArea(bounds, description.DesktopCoordinates);
        if (!SelectArea(area)) return true;
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
            const auto result = factory->EnumAdapters1(adapterIndex, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) return true;
            if (FAILED(result) || !adapter) return false;
            if (!ReadOutputs(adapter.Get())) return false;
        }
        return false;
    }

    bool Publish(DisplayCapabilities& capabilities) noexcept
    {
        if (ambiguous) return false;
        if (!candidate.known || !factory->IsCurrent()) return false;
        ReadDisplayConfiguration(candidate);
        if (!factory->IsCurrent()) return false;
        capabilities = candidate;
        return true;
    }
};

}  // namespace

DisplayCapabilities QueryDisplayCapabilities(HWND window) noexcept
{
    DisplayOutputSearch search;
    DisplayCapabilities capabilities;
    if (!search.ReadWindow(window)) return capabilities;
    if (!search.ReadAdapters()) return capabilities;
    search.Publish(capabilities);
    return capabilities;
}

void DisplayDiscovery::Reset() noexcept
{
    if (std::this_thread::get_id() != owner_) return;
    capabilities_ = {};
    factory_.Reset();
    monitor_ = nullptr;
    lastAttempt_ = 0;
    attempted_ = false;
}

bool DisplayDiscovery::MatchesMonitor(HMONITOR monitor) const noexcept
{
    if (monitor != monitor_) return false;
    return factory_ && factory_->IsCurrent();
}

bool DisplayDiscovery::BeginDiscovery() noexcept
{
    const auto now = GetTickCount64();
    if (attempted_ && now - lastAttempt_ < 1000) return false;
    attempted_ = true;
    lastAttempt_ = now;
    factory_.Reset();
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)))) return false;
    return factory_ != nullptr;
}

void DisplayDiscovery::Discover(HWND window) noexcept
{
    const auto candidate = QueryDisplayCapabilities(window);
    if (!candidate.known) return;
    if (candidate.description.Monitor != monitor_) return;
    if (!factory_->IsCurrent()) return;
    capabilities_ = candidate;
}

bool DisplayDiscovery::SelectMonitor(HMONITOR monitor) noexcept
{
    if (!monitor)
    {
        Reset();
        return false;
    }
    if (monitor != monitor_) Reset();
    if (factory_ && !factory_->IsCurrent()) Reset();
    monitor_ = monitor;
    return true;
}

DisplayCapabilities DisplayDiscovery::Refresh(HWND window) noexcept
{
    if (std::this_thread::get_id() != owner_) return {};
    const auto monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONULL) : nullptr;
    if (!SelectMonitor(monitor)) return {};
    if (MatchesMonitor(monitor) && capabilities_.known) return capabilities_;
    if (BeginDiscovery()) Discover(window);
    return capabilities_;
}

}  // namespace fo4hdr
