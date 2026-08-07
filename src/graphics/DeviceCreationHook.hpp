// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/graphics/DeviceCreationObservation.hpp"

#include <d3d11.h>

namespace fo4hdr {

struct CreatedDeviceBinding
{
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swapChain{};
    HRESULT result{};
    UINT flags{};
};

using CreatedDeviceObserver = bool (*)(const CreatedDeviceBinding& binding);
using CreationDisabledObserver = void (*)() noexcept;

struct DeviceCreationCallbacks
{
    DeviceCreationObserver facts{};
    CreatedDeviceObserver created{};
    CreationDisabledObserver disabled{};
};

// Returned interfaces are borrowed during delivery; a receiver owns any retained references.
[[nodiscard]] bool PrepareDeviceCreationObserver(DeviceCreationCallbacks callbacks) noexcept;
// Configuration, predecessor code and observer state remain alive until process exit.
[[nodiscard]] bool PrepareDeviceCreationObserver(DeviceCreationObserver observer) noexcept;
[[nodiscard]] bool PublishD3dNext(void* next) noexcept;
[[nodiscard]] bool DeviceCreationObservationActive() noexcept;

HRESULT WINAPI HookD3dCreate(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) noexcept;

}  // namespace fo4hdr
