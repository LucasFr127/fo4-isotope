// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/HookOwnership.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include "DeviceCreationHook.hpp"

namespace {

struct CreationRequest
{
    IDXGIAdapter* adapter{};
    D3D_DRIVER_TYPE driverType{};
    HMODULE software{};
    UINT flags{};
    const D3D_FEATURE_LEVEL* levels{};
    UINT levelCount{};
    UINT sdkVersion{};
    const DXGI_SWAP_CHAIN_DESC* description{};
    IDXGISwapChain** swapChain{};
    ID3D11Device** device{};
    D3D_FEATURE_LEVEL* featureLevel{};
    ID3D11DeviceContext** context{};
};

struct CreationEvidence
{
    unsigned nativeCalls{};
    unsigned observations{};
    HRESULT result{17};
    bool throwObserver{};
    bool throwNext{};
    CreationRequest captured;
    fo4hdr::DeviceCreationObservation observed;
    std::array<std::byte, 8> adapterToken{};
    std::array<std::byte, 8> softwareToken{};
    std::array<std::byte, 8> chainToken{};
    std::array<std::byte, 8> deviceToken{};
    std::array<std::byte, 8> contextToken{};
    IDXGISwapChain* chain{};
    IDXGISwapChain* ownedChain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL level{D3D_FEATURE_LEVEL_10_0};
    std::array<D3D_FEATURE_LEVEL, 2> requestedLevels{
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    DXGI_SWAP_CHAIN_DESC description{};
};

CreationEvidence evidence;
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN lifecycleCreation{};

IDXGISwapChain* ReturnedChain() noexcept
{
    if (evidence.ownedChain != nullptr)
    {
        return evidence.ownedChain;
    }
    return reinterpret_cast<IDXGISwapChain*>(evidence.chainToken.data());
}

ID3D11Device* ReturnedDevice() noexcept
{
    return reinterpret_cast<ID3D11Device*>(evidence.deviceToken.data());
}

ID3D11DeviceContext* ReturnedContext() noexcept
{
    return reinterpret_cast<ID3D11DeviceContext*>(evidence.contextToken.data());
}

CreationRequest MakeRequest()
{
    std::memset(&evidence.description, 0, sizeof(evidence.description));
    evidence.description.BufferDesc.Width = 641;
    evidence.description.BufferDesc.Height = 479;
    evidence.description.BufferCount = 3;
    return {reinterpret_cast<IDXGIAdapter*>(evidence.adapterToken.data()),
        D3D_DRIVER_TYPE_HARDWARE, reinterpret_cast<HMODULE>(evidence.softwareToken.data()),
        0x1234, evidence.requestedLevels.data(), 2, D3D11_SDK_VERSION,
        &evidence.description, &evidence.chain, &evidence.device, &evidence.level, &evidence.context};
}

template <class T>
void SetOptionalOutput(T* output, T value)
{
    if (output != nullptr)
    {
        *output = value;
    }
}

void FillOutputs(const CreationRequest& request)
{
    SetOptionalOutput(request.swapChain, ReturnedChain());
    SetOptionalOutput(request.device, ReturnedDevice());
    SetOptionalOutput(request.featureLevel, D3D_FEATURE_LEVEL_11_0);
    SetOptionalOutput(request.context, ReturnedContext());
}

HRESULT WINAPI ControlledCreation(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context)
{
    ++evidence.nativeCalls;
    evidence.captured = {adapter, driverType, software, flags, levels, levelCount, sdkVersion,
        description, swapChain, device, featureLevel, context};
    FillOutputs(evidence.captured);
    if (evidence.throwNext)
    {
        throw 19;
    }
    return evidence.result;
}

bool ObserveCreation(const fo4hdr::DeviceCreationObservation& event)
{
    ++evidence.observations;
    evidence.observed = event;
    if (evidence.throwObserver)
    {
        throw 23;
    }
    return true;
}

bool WriteOwnedSlot(void** slot, void* expected, void* value) noexcept
{
    return std::atomic_ref<void*>(*slot).compare_exchange_strong(expected, value);
}

HRESULT InvokeCreation(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN function,
    const CreationRequest& request)
{
    return function(request.adapter, request.driverType, request.software, request.flags,
        request.levels, request.levelCount, request.sdkVersion, request.description,
        request.swapChain, request.device, request.featureLevel, request.context);
}

void CheckCapturedRequest(const CreationRequest& request)
{
    const auto& captured = evidence.captured;
    FO4HDR_CHECK(captured.adapter == request.adapter);
    FO4HDR_CHECK(captured.driverType == request.driverType);
    FO4HDR_CHECK(captured.software == request.software);
    FO4HDR_CHECK(captured.flags == request.flags);
    FO4HDR_CHECK(captured.levels == request.levels);
    FO4HDR_CHECK(captured.levelCount == request.levelCount);
    FO4HDR_CHECK(captured.sdkVersion == request.sdkVersion);
    FO4HDR_CHECK(captured.description == request.description);
    FO4HDR_CHECK(captured.swapChain == request.swapChain);
    FO4HDR_CHECK(captured.device == request.device);
    FO4HDR_CHECK(captured.featureLevel == request.featureLevel);
    FO4HDR_CHECK(captured.context == request.context);
}

void CheckReturnedOutputs(const CreationRequest& request)
{
    if (request.swapChain != nullptr)
    {
        FO4HDR_CHECK(*request.swapChain == ReturnedChain());
    }
    if (request.device != nullptr)
    {
        FO4HDR_CHECK(*request.device == ReturnedDevice());
    }
    if (request.featureLevel != nullptr)
    {
        FO4HDR_CHECK(*request.featureLevel == D3D_FEATURE_LEVEL_11_0);
    }
    if (request.context != nullptr)
    {
        FO4HDR_CHECK(*request.context == ReturnedContext());
    }
}

void CheckCreation(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN function,
    const CreationRequest& request, HRESULT expected)
{
    const auto calls = evidence.nativeCalls;
    std::array<std::byte, sizeof(DXGI_SWAP_CHAIN_DESC)> descriptorBytes{};
    if (request.description != nullptr)
    {
        std::memcpy(descriptorBytes.data(), request.description, descriptorBytes.size());
    }
    FO4HDR_CHECK(InvokeCreation(function, request) == expected);
    FO4HDR_CHECK(evidence.nativeCalls == calls + 1);
    CheckCapturedRequest(request);
    CheckReturnedOutputs(request);
    if (request.description != nullptr)
    {
        FO4HDR_CHECK(std::memcmp(descriptorBytes.data(), request.description,
            descriptorBytes.size()) == 0);
    }
    FO4HDR_CHECK(evidence.requestedLevels[0] == D3D_FEATURE_LEVEL_11_0);
    FO4HDR_CHECK(evidence.requestedLevels[1] == D3D_FEATURE_LEVEL_10_0);
}

void CheckFailedAndSuccessfulCreation(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN function)
{
    const auto request = MakeRequest();
    evidence.result = E_FAIL;
    CheckCreation(function, request, E_FAIL);
    FO4HDR_CHECK(evidence.observations == 0);
    evidence.result = 17;
    CheckCreation(function, request, 17);
    FO4HDR_CHECK(evidence.observations == 1);
    FO4HDR_CHECK(evidence.observed.flags == request.flags);
    FO4HDR_CHECK(evidence.observed.swapChainReturned);
    FO4HDR_CHECK(evidence.observed.deviceReturned);
    FO4HDR_CHECK(evidence.observed.contextReturned);
}

void CheckOptionalOutputs(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN function)
{
    auto request = MakeRequest();
    request.swapChain = nullptr;
    request.description = nullptr;
    request.featureLevel = nullptr;
    CheckCreation(function, request, 17);
    FO4HDR_CHECK(evidence.observations == 2);
    FO4HDR_CHECK(!evidence.observed.swapChainReturned);
    FO4HDR_CHECK(evidence.observed.deviceReturned);
    FO4HDR_CHECK(evidence.observed.contextReturned);
    request.device = nullptr;
    request.context = nullptr;
    CheckCreation(function, request, 17);
    FO4HDR_CHECK(evidence.observations == 2);
}

void CheckObserverFailure(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN function)
{
    const auto request = MakeRequest();
    const auto observations = evidence.observations;
    evidence.throwObserver = true;
    CheckCreation(function, request, 17);
    FO4HDR_CHECK(evidence.observations == observations + 1);
    FO4HDR_CHECK(!fo4hdr::DeviceCreationObservationActive());
    CheckCreation(function, request, 17);
    FO4HDR_CHECK(evidence.observations == observations + 1);
    evidence.throwNext = true;
    CheckCreation(function, request, E_FAIL);
    FO4HDR_CHECK(evidence.observations == observations + 1);
}

void CreateLifecycleChain(IDXGISwapChain* chain)
{
    evidence.ownedChain = chain;
    const auto observations = evidence.observations;
    CheckCreation(lifecycleCreation, MakeRequest(), 17);
    FO4HDR_CHECK(evidence.observations == observations + 1);
    evidence.ownedChain = nullptr;
}

}  // namespace

namespace fo4hdr::tests {

void RunDeviceCreationHookTests()
{
    const auto request = MakeRequest();
    FO4HDR_CHECK(InvokeCreation(HookD3dCreate, request) == E_UNEXPECTED);
    FO4HDR_CHECK(evidence.nativeCalls == 0);
    FO4HDR_CHECK(!PublishD3dNext(reinterpret_cast<void*>(&ControlledCreation)));
    FO4HDR_CHECK(!PrepareDeviceCreationObserver(nullptr));
    FO4HDR_CHECK(PrepareDeviceCreationObserver(ObserveCreation));
    FO4HDR_CHECK(!PrepareDeviceCreationObserver(ObserveCreation));
    FO4HDR_CHECK(!PublishD3dNext(nullptr));
    FO4HDR_CHECK(!PublishD3dNext(reinterpret_cast<void*>(&HookD3dCreate)));
    void* slot = reinterpret_cast<void*>(&ControlledCreation);
    PointerHookOwnership owner;
    FO4HDR_CHECK(owner.Install(&slot, reinterpret_cast<void*>(&HookD3dCreate),
        WriteOwnedSlot, PublishD3dNext));
    FO4HDR_CHECK(owner.State() == HookOwnershipState::owned);
    FO4HDR_CHECK(owner.Next() == reinterpret_cast<void*>(&ControlledCreation));
    const auto function = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(slot);
    CheckFailedAndSuccessfulCreation(function);
    CheckOptionalOutputs(function);
    lifecycleCreation = function;
    RunSwapChainObservationTests(CreateLifecycleChain);
    CheckObserverFailure(function);
    FO4HDR_CHECK(owner.Remove(WriteOwnedSlot));
    FO4HDR_CHECK(slot == reinterpret_cast<void*>(&ControlledCreation));
}

}  // namespace fo4hdr::tests
