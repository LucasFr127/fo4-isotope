// SPDX-License-Identifier: GPL-3.0-or-later

#include "DeviceCreationHook.hpp"
#include "SwapChainObservation.hpp"

#include <Windows.h>

#include <atomic>
#include <type_traits>

namespace fo4hdr {
namespace {

std::atomic_flag preparationClaim = ATOMIC_FLAG_INIT;
std::atomic<DeviceCreationObserver> creationObserver{};
std::atomic<CreatedDeviceObserver> typedObserver{};
std::atomic<CreationDisabledObserver> disabledObserver{};
std::atomic<CreatedDeviceObserver> hdrObserver{};
std::atomic<DiscardHdrCreation> discardHdrObserver{};
std::atomic<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN> capturedNext{};
std::atomic<bool> observationActive{};
std::atomic<bool> hdrEnabled{true}, forceHdrFailure{};
std::atomic_flag observationBusy = ATOMIC_FLAG_INIT;
std::atomic_flag hdrAttemptClaim = ATOMIC_FLAG_INIT;

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

void DisableObservation() noexcept
{
    observationActive.store(false, std::memory_order_release);
    if (const auto disabled = disabledObserver.load(std::memory_order_acquire))
    {
        disabled();
    }
}

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
        DisableObservation();
        return E_FAIL;
    }
}

bool OwnedCreationWindow(HWND window) noexcept
{
    if (window == nullptr || !IsWindow(window)) return false;
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(window, &process);
    return thread == GetCurrentThreadId() && process == GetCurrentProcessId();
}

bool HdrCallbacksAvailable() noexcept
{
    return observationActive.load(std::memory_order_acquire) &&
        hdrObserver.load(std::memory_order_acquire) != nullptr &&
        discardHdrObserver.load(std::memory_order_acquire) != nullptr;
}

bool HasCreationOutputSlots(const CreationArguments& arguments) noexcept
{
    return arguments.swapChain != nullptr && arguments.device != nullptr && arguments.context != nullptr;
}

bool HdrCreationWindowLayout(const DXGI_SWAP_CHAIN_DESC& value) noexcept
{
    return OwnedCreationWindow(value.OutputWindow) && value.Windowed &&
        value.BufferDesc.Width != 0 && value.BufferDesc.Height != 0;
}

bool HdrCreationStorageLayout(const DXGI_SWAP_CHAIN_DESC& value) noexcept
{
    return value.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        value.SampleDesc.Count == 1 && value.SampleDesc.Quality == 0 &&
        (value.BufferUsage & DXGI_USAGE_SHADER_INPUT) == 0;
}

bool HdrCreationEligible(const CreationArguments& arguments) noexcept
{
    if (!hdrEnabled.load(std::memory_order_acquire) ||
        forceHdrFailure.load(std::memory_order_acquire)) return false;
    if (arguments.description == nullptr) return false;
    return HdrCallbacksAvailable() && HasCreationOutputSlots(arguments) &&
        HdrCreationWindowLayout(*arguments.description) && HdrCreationStorageLayout(*arguments.description);
}

void ResetCreationOutputs(const CreationArguments& arguments) noexcept
{
    *arguments.swapChain = nullptr;
    *arguments.device = nullptr;
    *arguments.context = nullptr;
    if (arguments.featureLevel != nullptr) *arguments.featureLevel = D3D_FEATURE_LEVEL{};
}

void ReleaseCreationOutputs(const CreationArguments& arguments, bool flush) noexcept
{
    auto* context = *arguments.context;
    if (*arguments.swapChain != nullptr) (*arguments.swapChain)->Release();
    if (*arguments.device != nullptr) (*arguments.device)->Release();
    ResetCreationOutputs(arguments);
    // A rejected flip chain must be destroyed before its same-window retry.
    if (context != nullptr)
    {
        if (flush) context->Flush();
        context->Release();
    }
}

bool CompleteCreationOutputs(const CreationArguments& arguments) noexcept
{
    return *arguments.swapChain != nullptr && *arguments.device != nullptr && *arguments.context != nullptr;
}

bool DeliverHdrCreation(const CreatedDeviceBinding& created) noexcept
{
    try
    {
        const auto observer = hdrObserver.load(std::memory_order_acquire);
        return observer != nullptr && observer(created);
    }
    catch (...) { return false; }
}

bool PrepareHdrOutputs(const CreationArguments& arguments, HRESULT result) noexcept
{
    if (FAILED(result)) return false;
    if (!CompleteCreationOutputs(arguments)) return false;
    if (!observationActive.load(std::memory_order_acquire)) return false;
    const CreatedDeviceBinding created{*arguments.device, *arguments.context,
        *arguments.swapChain, result, arguments.flags};
    return DeliverHdrCreation(created) && observationActive.load(std::memory_order_acquire);
}

struct CreationResult
{
    HRESULT result{};
    bool hdrAccepted{};
};

CreationResult CreateHdrOrOriginal(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN next,
    const CreationArguments& original) noexcept
{
    if (!HdrCreationEligible(original) || hdrAttemptClaim.test_and_set(std::memory_order_acq_rel))
        return {ForwardCreation(next, original), false};
    auto modified = *original.description;
    modified.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    modified.BufferUsage |= DXGI_USAGE_UNORDERED_ACCESS | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    modified.BufferCount = 2;
    modified.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    auto candidate = original;
    candidate.description = &modified;
    ResetCreationOutputs(candidate);
    const auto result = ForwardCreation(next, candidate);
    if (PrepareHdrOutputs(candidate, result)) return {result, true};
    const auto discard = discardHdrObserver.load(std::memory_order_acquire);
    const CreatedDeviceBinding rejected{*candidate.device, *candidate.context,
        *candidate.swapChain, result, candidate.flags};
    const bool clean = discard != nullptr && discard(rejected);
    ReleaseCreationOutputs(candidate, clean && SUCCEEDED(result));
    if (!clean)
    {
        // Ambiguous reachable callbacks retain their resident state; no retry may
        // assume that the candidate and its indirect aliases were destroyed.
        DisableObservation();
        return {E_FAIL, false};
    }
    return {ForwardCreation(next, original), false};
}

bool HasReturnedObject(const DeviceCreationObservation& event) noexcept
{
    return event.swapChainReturned || event.deviceReturned || event.contextReturned;
}

bool DeliverObservation(const CreationArguments& arguments,
    const DeviceCreationObservation& event, HRESULT result) noexcept
{
    try
    {
        if (const auto created = typedObserver.load(std::memory_order_acquire))
        {
            const CreatedDeviceBinding binding{
                event.deviceReturned ? *arguments.device : nullptr,
                event.contextReturned ? *arguments.context : nullptr,
                event.swapChainReturned ? *arguments.swapChain : nullptr, result, event.flags};
            if (!created(binding))
            {
                return false;
            }
        }
        const auto facts = creationObserver.load(std::memory_order_acquire);
        return facts == nullptr || facts(event);
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

void ObserveSuccessfulCreation(const CreationArguments& arguments, HRESULT result) noexcept
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
    // The copied-facts fixture route is absent from the retained game-device callback setup.
    if (event.swapChainReturned && creationObserver.load(std::memory_order_acquire) != nullptr)
    {
        OfferCreatedSwapChain(*arguments.swapChain);
    }
    if (!DeliverObservation(arguments, event, result))
    {
        DisableObservation();
    }
}

}  // namespace

namespace {

bool ValidHdrCallbacks(const DeviceCreationCallbacks& callbacks) noexcept
{
    if (callbacks.hdrCreated == nullptr && callbacks.discardHdr == nullptr) return true;
    return callbacks.hdrCreated != nullptr && callbacks.discardHdr != nullptr &&
        callbacks.created != nullptr && callbacks.facts == nullptr;
}

}  // namespace

bool PrepareDeviceCreationObserver(DeviceCreationObserver observer) noexcept
{
    return PrepareDeviceCreationObserver(DeviceCreationCallbacks{observer});
}

bool PrepareDeviceCreationObserver(DeviceCreationCallbacks callbacks) noexcept
{
    if (callbacks.facts == nullptr && callbacks.created == nullptr)
    {
        return false;
    }
    if (!ValidHdrCallbacks(callbacks)) return false;
    if (preparationClaim.test_and_set(std::memory_order_acq_rel))
    {
        return false;
    }
    observationActive.store(true, std::memory_order_relaxed);
    creationObserver.store(callbacks.facts, std::memory_order_release);
    typedObserver.store(callbacks.created, std::memory_order_release);
    disabledObserver.store(callbacks.disabled, std::memory_order_release);
    hdrObserver.store(callbacks.hdrCreated, std::memory_order_release);
    discardHdrObserver.store(callbacks.discardHdr, std::memory_order_release);
    hdrEnabled.store(callbacks.hdrEnabled, std::memory_order_release);
    forceHdrFailure.store(callbacks.forceHdrFailure, std::memory_order_release);
    return true;
}

bool PublishD3dNext(void* next) noexcept
{
    if (next == nullptr || next == reinterpret_cast<void*>(&HookD3dCreate))
    {
        return false;
    }
    if (creationObserver.load(std::memory_order_acquire) == nullptr &&
        typedObserver.load(std::memory_order_acquire) == nullptr)
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

namespace {

bool ShouldObserveCreation(bool entered, const CreationResult& creation) noexcept
{
    return entered && SUCCEEDED(creation.result) && !creation.hdrAccepted;
}

HRESULT InvokeCreation(const CreationArguments& arguments) noexcept
{
    const auto next = capturedNext.load(std::memory_order_acquire);
    if (next == nullptr)
    {
        return E_UNEXPECTED;
    }
    // Native creation still forwards on overlap; only the optional observation is revoked.
    const auto entered = !observationBusy.test_and_set(std::memory_order_acquire);
    if (!entered)
    {
        DisableObservation();
    }
    const auto creation = entered ? CreateHdrOrOriginal(next, arguments) :
        CreationResult{ForwardCreation(next, arguments), false};
    if (ShouldObserveCreation(entered, creation))
    {
        ObserveSuccessfulCreation(arguments, creation.result);
    }
    if (entered)
    {
        observationBusy.clear(std::memory_order_release);
    }
    return creation.result;
}

}  // namespace

HRESULT WINAPI HookD3dCreate(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) noexcept
{
    const CreationArguments arguments{adapter, driverType, software, flags, levels, levelCount,
        sdkVersion, description, swapChain, device, featureLevel, context};
    return InvokeCreation(arguments);
}

static_assert(std::is_convertible_v<decltype(&HookD3dCreate),
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>);

}  // namespace fo4hdr
