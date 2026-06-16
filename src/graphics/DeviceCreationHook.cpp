// SPDX-License-Identifier: GPL-3.0-or-later

#include "DeviceCreationHook.hpp"
#include "SwapChainObservation.hpp"

#include <atomic>
#include <type_traits>

namespace fo4hdr {
namespace {

std::atomic_flag preparationClaim = ATOMIC_FLAG_INIT;
std::atomic<DeviceCreationObserver> creationObserver{};
std::atomic<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN> capturedNext{};
std::atomic<bool> observationActive{};
std::atomic_flag observationBusy = ATOMIC_FLAG_INIT;

struct CreationArguments
{
    IDXGIAdapter* adapter;
    D3D_DRIVER_TYPE driverType;
    HMODULE software;
    UINT flags;
    const D3D_FEATURE_LEVEL* levels;
    UINT levelCount;
    UINT sdkVersion;
    const DXGI_SWAP_CHAIN_DESC* description;
    IDXGISwapChain** swapChain;
    ID3D11Device** device;
    D3D_FEATURE_LEVEL* featureLevel;
    ID3D11DeviceContext** context;
};

HRESULT ForwardCreation(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN next,
    const CreationArguments& arguments) noexcept
{
    try
    {
        return next(arguments.adapter, arguments.driverType, arguments.software,
            arguments.flags, arguments.levels, arguments.levelCount, arguments.sdkVersion,
            arguments.description, arguments.swapChain, arguments.device,
            arguments.featureLevel, arguments.context);
    }
    catch (...)
    {
        observationActive.store(false, std::memory_order_release);
        return E_FAIL;
    }
}

bool HasReturnedObject(const DeviceCreationObservation& event) noexcept
{
    return event.swapChainReturned || event.deviceReturned || event.contextReturned;
}

bool DeliverObservation(const DeviceCreationObservation& event) noexcept
{
    try
    {
        return creationObserver.load(std::memory_order_acquire)(event);
    }
    catch (...)
    {
        return false;
    }
}

DeviceCreationObservation CaptureCreatedOutputs(const CreationArguments& arguments) noexcept
{
    return {arguments.flags,
        arguments.swapChain != nullptr && *arguments.swapChain != nullptr,
        arguments.device != nullptr && *arguments.device != nullptr,
        arguments.context != nullptr && *arguments.context != nullptr};
}

void ObserveSuccessfulCreation(const CreationArguments& arguments) noexcept
{
    if (!observationActive.load(std::memory_order_acquire))
    {
        return;
    }
    const auto event = CaptureCreatedOutputs(arguments);
    if (!HasReturnedObject(event))
    {
        return;
    }
    // An overlapping/reentrant observer has no proven ownership; stop without waiting.
    if (observationBusy.test_and_set(std::memory_order_acquire))
    {
        observationActive.store(false, std::memory_order_release);
        return;
    }
    if (!observationActive.load(std::memory_order_acquire))
    {
        observationBusy.clear(std::memory_order_release);
        return;
    }
    if (event.swapChainReturned)
    {
        OfferCreatedSwapChain(*arguments.swapChain);
    }
    if (!DeliverObservation(event))
    {
        observationActive.store(false, std::memory_order_release);
    }
    observationBusy.clear(std::memory_order_release);
}

}  // namespace

bool PrepareDeviceCreationObserver(DeviceCreationObserver observer) noexcept
{
    if (observer == nullptr)
    {
        return false;
    }
    if (preparationClaim.test_and_set(std::memory_order_acq_rel))
    {
        return false;
    }
    observationActive.store(true, std::memory_order_relaxed);
    creationObserver.store(observer, std::memory_order_release);
    return true;
}

bool PublishD3dNext(void* next) noexcept
{
    if (next == nullptr || next == reinterpret_cast<void*>(&HookD3dCreate))
    {
        return false;
    }
    if (creationObserver.load(std::memory_order_acquire) == nullptr)
    {
        return false;
    }
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN expected{};
    return capturedNext.compare_exchange_strong(expected,
        reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(next),
        std::memory_order_release, std::memory_order_relaxed);
}

bool DeviceCreationObservationActive() noexcept
{
    return observationActive.load(std::memory_order_acquire);
}

HRESULT WINAPI HookD3dCreate(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) noexcept
{
    const auto next = capturedNext.load(std::memory_order_acquire);
    if (next == nullptr)
    {
        return E_UNEXPECTED;
    }
    const CreationArguments arguments{adapter, driverType, software, flags, levels, levelCount,
        sdkVersion, description, swapChain, device, featureLevel, context};
    const auto result = ForwardCreation(next, arguments);
    if (SUCCEEDED(result))
    {
        ObserveSuccessfulCreation(arguments);
    }
    return result;
}

static_assert(std::is_convertible_v<decltype(&HookD3dCreate),
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>);

}  // namespace fo4hdr
