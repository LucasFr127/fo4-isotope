// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameDeviceGeneration.hpp"
#include "DeviceCreationHook.hpp"
#include "hooks/ContextHooks.hpp"

#include "Fallout4HDR/hooks/HookOwnership.hpp"
#include "Fallout4HDR/hooks/PeIat.hpp"

#include <Windows.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t disabledToken = std::uint64_t{1} << 63;
constexpr std::size_t maximumImageBytes = 0x80000000;

struct DeviceCandidate
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
};

struct RetainedBinding
{
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* chain{};
    DWORD thread{};
    std::uint64_t lastGeneration{};
    ContextQueryPreparation queryPreparation{ContextQueryPreparation::invalidContext};

    void ReleaseOwned() noexcept
    {
        if (chain != nullptr) chain->Release();
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
        chain = nullptr;
        context = nullptr;
        device = nullptr;
    }

    void Retain(DeviceCandidate& candidate) noexcept
    {
        device = candidate.device.Detach();
        context = candidate.context.Detach();
        chain = candidate.chain.Detach();
    }
};

// Raw owned references deliberately have no shutdown destructor. Ambiguous callback
// ownership revokes publication but retains at most this one binding until process exit.
RetainedBinding binding;
ContextQueryObserver queryObserver;
PointerHookOwnership importOwner;
std::atomic_flag initialization = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> publication{};
void* expectedExport{};

void DisableBinding() noexcept
{
    static_cast<void>(publication.exchange(disabledToken, std::memory_order_acq_rel));
}

ComPtr<IUnknown> CanonicalIdentity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object == nullptr) return identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) identity.Reset();
    return identity;
}

bool SameIdentity(IUnknown* left, IUnknown* right) noexcept
{
    const auto leftIdentity = CanonicalIdentity(left);
    const auto rightIdentity = CanonicalIdentity(right);
    return leftIdentity && leftIdentity.Get() == rightIdentity.Get();
}

bool QualifyContext(const DeviceCandidate& candidate) noexcept
{
    if (candidate.device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0 ||
        candidate.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> contextDevice;
    ComPtr<ID3D11DeviceContext> immediate;
    candidate.context->GetDevice(&contextDevice);
    candidate.device->GetImmediateContext(&immediate);
    return SameIdentity(candidate.device.Get(), contextDevice.Get()) &&
        SameIdentity(candidate.context.Get(), immediate.Get());
}

bool QualifyChain(const DeviceCandidate& candidate) noexcept
{
    if (!candidate.chain) return true;
    ComPtr<ID3D11Device> chainDevice;
    DXGI_SWAP_CHAIN_DESC description{};
    return SUCCEEDED(candidate.chain->GetDevice(IID_PPV_ARGS(&chainDevice))) &&
        SameIdentity(candidate.device.Get(), chainDevice.Get()) &&
        SUCCEEDED(candidate.chain->GetDesc(&description)) &&
        description.BufferDesc.Width != 0 && description.BufferDesc.Height != 0;
}

bool CaptureCandidate(const CreatedDeviceBinding& created, DeviceCandidate& candidate) noexcept
{
    if (created.device == nullptr || created.context == nullptr) return false;
    candidate.device = created.device;
    candidate.context = created.context;
    candidate.chain = created.swapChain;
    return QualifyContext(candidate) && QualifyChain(candidate);
}

bool RevokeForReplacement(DWORD thread) noexcept
{
    if (binding.thread != 0 && binding.thread != thread)
    {
        DisableBinding();
        return false;
    }
    auto observed = publication.load(std::memory_order_acquire);
    if (observed == disabledToken) return false;
    return publication.compare_exchange_strong(observed, 0, std::memory_order_acq_rel);
}

bool ObserveDevice(const CreatedDeviceBinding& created) noexcept
{
    const auto thread = GetCurrentThreadId();
    if (!RevokeForReplacement(thread)) return false;
    if (!queryObserver.Retire())
    {
        DisableBinding();
        return false;
    }
    binding.thread = thread;
    binding.ReleaseOwned();
    DeviceCandidate candidate;
    if (!CaptureCandidate(created, candidate)) return true;
    if (binding.lastGeneration == disabledToken - 1)
    {
        DisableBinding();
        return false;
    }
    const auto generation = ++binding.lastGeneration;
    binding.queryPreparation = queryObserver.Prepare(candidate.context.Get(), generation);
    if (binding.queryPreparation == ContextQueryPreparation::invalidContext) return true;
    // This token publishes device identity only; unobserved query coverage stays passive.
    binding.Retain(candidate);
    std::uint64_t expected{};
    if (!publication.compare_exchange_strong(expected, generation, std::memory_order_release))
    {
        if (!queryObserver.Retire())
        {
            DisableBinding();
            return false;
        }
        binding.ReleaseOwned();
        return false;
    }
    return true;
}

bool ReadableProtection(DWORD protection) noexcept
{
    if ((protection & PAGE_GUARD) != 0) return false;
    constexpr std::array<DWORD, 6> readable{PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY,
        PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY};
    return std::find(readable.begin(), readable.end(), protection & 0xFF) != readable.end();
}

bool ImageRegion(const void* address, const void* base,
    MEMORY_BASIC_INFORMATION& region) noexcept
{
    return VirtualQuery(address, &region, sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT && region.Type == MEM_IMAGE &&
        region.AllocationBase == base && ReadableProtection(region.Protect);
}

bool ImageExtentReadable(std::byte* base, std::size_t bytes) noexcept
{
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    if (bytes > std::numeric_limits<std::uintptr_t>::max() - start) return false;
    const auto end = start + bytes;
    auto cursor = start;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!ImageRegion(reinterpret_cast<void*>(cursor), base, region)) return false;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBase)
            return false;
        const auto next = regionBase + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

template <class Value>
bool ReadHeader(std::span<std::byte> header, std::size_t offset, Value& value) noexcept
{
    if (offset > header.size() || sizeof(Value) > header.size() - offset) return false;
    std::memcpy(&value, header.data() + offset, sizeof(Value));
    return true;
}

bool ReadNtOffset(std::span<std::byte> header, std::uint32_t& nt) noexcept
{
    std::uint16_t dos{};
    return ReadHeader(header, 0, dos) && dos == IMAGE_DOS_SIGNATURE &&
        ReadHeader(header, 0x3C, nt);
}

bool ReadImageSize(std::span<std::byte> header, std::uint32_t nt,
    std::uint32_t& bytes) noexcept
{
    std::uint32_t signature{};
    std::uint16_t optionalSize{};
    const auto offset = static_cast<std::size_t>(nt);
    return ReadHeader(header, offset, signature) && signature == IMAGE_NT_SIGNATURE &&
        ReadHeader(header, offset + 20, optionalSize) && optionalSize >= 64 &&
        ReadHeader(header, offset + 80, bytes);
}

std::size_t ImageSize(std::span<std::byte> header) noexcept
{
    std::uint32_t nt{};
    std::uint32_t bytes{};
    if (!ReadNtOffset(header, nt)) return 0;
    if (!ReadImageSize(header, nt, bytes)) return 0;
    return bytes >= 4096 && bytes <= maximumImageBytes ? bytes : 0;
}

wchar_t LowerAscii(wchar_t character) noexcept
{
    return character >= L'A' && character <= L'Z' ? character + 32 : character;
}

bool MainExecutableNamed() noexcept
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return false;
    std::wstring_view name(path.data(), length);
    name.remove_prefix(name.find_last_of(L"/\\") + 1);
    constexpr std::wstring_view expected = L"fallout4.exe";
    return std::equal(name.begin(), name.end(), expected.begin(), expected.end(),
        [](wchar_t left, wchar_t right) { return LowerAscii(left) == right; });
}

std::span<std::byte> MainImage() noexcept
{
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    MEMORY_BASIC_INFORMATION first{};
    if (base == nullptr) return {};
    if (!MainExecutableNamed()) return {};
    if (!ImageRegion(base, base, first)) return {};
    const auto bytes = ImageSize({base, first.RegionSize});
    if (bytes == 0 || !ImageExtentReadable(base, bytes)) return {};
    return {base, bytes};
}

bool PublishExpectedNext(void* next) noexcept
{
    return next == expectedExport && PublishD3dNext(next);
}

bool ResolveExpectedExport(void** slot) noexcept
{
    const auto module = GetModuleHandleW(L"d3d11.dll");
    if (module == nullptr) return false;
    expectedExport = reinterpret_cast<void*>(GetProcAddress(module, "D3D11CreateDeviceAndSwapChain"));
    if (!IsExecutableHookTarget(expectedExport)) return false;
    return AtomicPointerLoad(slot) == expectedExport;
}

GraphicsDeviceBindingStatus InstallImport(std::span<std::byte> image) noexcept
{
    const auto imported = FindPe32PlusIatSlot(image, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    if (imported.status != PeIatStatus::success) return GraphicsDeviceBindingStatus::importUnavailable;
    if (!ResolveExpectedExport(imported.slot))
        return GraphicsDeviceBindingStatus::foreignPredecessor;
    if (!PrepareDeviceCreationObserver(DeviceCreationCallbacks{nullptr, ObserveDevice, DisableBinding}))
        return GraphicsDeviceBindingStatus::installationFailed;
    const auto writer = static_cast<HookPointerWriter>(&WriteProtectedPointer);
    if (!importOwner.Install(imported.slot, reinterpret_cast<void*>(&HookD3dCreate),
            writer, PublishExpectedNext))
    {
        DisableBinding();
        return GraphicsDeviceBindingStatus::installationFailed;
    }
    return GraphicsDeviceBindingStatus::installed;
}

}  // namespace

GraphicsDeviceBindingStatus InitializeGameDeviceGeneration() noexcept
{
    if (initialization.test_and_set(std::memory_order_acq_rel))
        return GraphicsDeviceBindingStatus::alreadyInitialized;
    const auto image = MainImage();
    if (image.empty()) return GraphicsDeviceBindingStatus::imageUnavailable;
    return InstallImport(image);
}

}  // namespace fo4hdr
