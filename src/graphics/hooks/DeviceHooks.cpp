// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "DeviceHooks.hpp"

#include "HookRecords.hpp"
#include "../FrameTrace.hpp"
#include "../SharedUiFrame.hpp"
#include "ContextHooks.hpp"
#include "SwapChainHooks.hpp"
#include "../DisplayDiscovery.hpp"
#include "../resources/ReplacementShaders.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include "FO4Isotope/hooks/PeIat.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LiveSettings.hpp"
#include "../RendererLifetime.hpp"
#include "../ShaderRegistry.hpp"
#include "../resources/ScenePromotionPolicy.hpp"
#include "../resources/PresentationResources.hpp"
#include "../d3d11/PresentationSwapChain.hpp"

#include <wrl/client.h>
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace fo4hdr {

using Microsoft::WRL::ComPtr;

std::array<DeviceRecord, kMaximumHookObjects> g_devices{};
std::array<ContextRecord, kMaximumHookObjects> g_contexts{};
std::array<SwapChainRecord, kMaximumHookObjects> g_swapChains{};
std::atomic_flag g_installLock = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> g_objectGeneration{};

template <class Record, std::size_t Count>
[[nodiscard]] Record* FindRecord(
    std::array<Record, Count>& records, void* object) noexcept
{
    for (auto& record : records)
        if (record.identity.Matches(object)) return &record;
    return nullptr;
}

[[nodiscard]] DeviceRecord* FindDevice(ID3D11Device* device) noexcept
{
    return FindRecord(g_devices, device);
}

[[nodiscard]] ContextRecord* FindContext(
    ID3D11DeviceContext* context) noexcept
{
    return FindRecord(g_contexts, context);
}

[[nodiscard]] SwapChainRecord* FindSwapChain(
    IDXGISwapChain* swapChain) noexcept
{
    return FindRecord(g_swapChains, swapChain);
}

DrawObservation* FindDrawObservation(ID3D11DeviceContext* context) noexcept
{
    return FindContext(context);
}

RenderTargetSetter OriginalRenderTargetSetter(ID3D11DeviceContext* context) noexcept
{
    const auto* record = FindContext(context);
    return record ? record->nextOmSetRenderTargets : nullptr;
}

bool HasOriginalPixelShaderSetter(ID3D11DeviceContext* context) noexcept
{
    const auto* record = FindContext(context);
    return record && record->nextPsSetShader;
}

void BindOriginalPixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader) noexcept
{
    const auto* record = FindContext(context);
    if (record && record->nextPsSetShader)
        record->nextPsSetShader(context, shader, nullptr, 0);
}

static bool QueryCommittedRegion(const void* address, MEMORY_BASIC_INFORMATION& info) noexcept
{
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT;
}

static bool RegionAccessible(const MEMORY_BASIC_INFORMATION& info) noexcept
{
    return (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}
static bool QueryAccessibleRegion(const void* address, MEMORY_BASIC_INFORMATION& info) noexcept
{
    return QueryCommittedRegion(address, info) && RegionAccessible(info);
}

static bool RegionContains(const MEMORY_BASIC_INFORMATION& info, const void* address, std::size_t bytes) noexcept
{
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return begin >= region && begin - region <= info.RegionSize &&
        bytes <= info.RegionSize - (begin - region);
}
[[nodiscard]] bool IsReadable(const void* address, std::size_t bytes) noexcept
{
    if (address == nullptr || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!QueryAccessibleRegion(address, info))
        return false;
    return RegionContains(info, address, bytes);
}

[[nodiscard]] static bool IsCommittedAccessible(
    const void* address, std::size_t bytes) noexcept
{
    auto current = reinterpret_cast<std::uintptr_t>(address);
    const auto end = current + bytes;
    if (end < current) return false;
    while (current < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!QueryAccessibleRegion(reinterpret_cast<const void*>(current), info))
            return false;
        const auto regionEnd =
            reinterpret_cast<std::uintptr_t>(info.BaseAddress) +
            info.RegionSize;
        if (regionEnd <= current) return false;
        current = regionEnd;
    }
    return true;
}

[[nodiscard]] bool IsExecutableAddress(const void* address) noexcept
{
    if (address == nullptr) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!QueryAccessibleRegion(address, info))
        return false;
    constexpr std::array<DWORD, 4> executableProtections{
        PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY};
    return std::ranges::find(executableProtections, info.Protect & 0xFFu) != executableProtections.end();
}

static bool CallableVtableSlot(void* const* slot) noexcept
{
    return IsReadable(slot, sizeof(void*)) && IsExecutableAddress(*slot);
}
// Preserve the complete contiguous callable tail exposed by the concrete
// driver object; a fixed public-interface length can truncate vendor-private
// entries and dispatch through a null slot.
[[nodiscard]] std::size_t DiscoverVtableSlots(
    void* const* table, std::size_t minimum) noexcept
{
    if (table == nullptr) return 0;
    std::size_t count{};
    for (; count < kMaximumVtableSlots; ++count)
    {
        if (!CallableVtableSlot(table + count))
            break;
    }
    if (count == kMaximumVtableSlots &&
        CallableVtableSlot(table + count))
        return 0;
    return count >= minimum ? count : 0;
}

[[nodiscard]] bool ObjectCompareExchange(
    void** slot, void* expected, void* value) noexcept
{
    if (slot == nullptr) return false;
    std::atomic_ref<void*> atomic(*slot);
    return atomic.compare_exchange_strong(
        expected, value, std::memory_order_acq_rel);
}

[[nodiscard]] bool ProtectedCompareExchange(
    void** slot, void* expected, void* value) noexcept
{
    if (slot == nullptr) return false;
    DWORD originalProtection{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE,
            &originalProtection))
        return false;
    std::atomic_ref<void*> atomic(*slot);
    const auto exchanged = atomic.compare_exchange_strong(
        expected, value, std::memory_order_acq_rel);
    DWORD ignored{};
    if (VirtualProtect(slot, sizeof(void*), originalProtection, &ignored))
        return exchanged;

    g_evidence.protectionRestoreFailureCount.fetch_add(
        1, std::memory_order_relaxed);
    RecordFailure(GraphicsObservationFailure::protectionRestoreFailed);
    if (exchanged)
    {
        auto installed = value;
        (void)atomic.compare_exchange_strong(
            installed, expected, std::memory_order_acq_rel);
    }
    (void)VirtualProtect(slot, sizeof(void*), originalProtection, &ignored);
    return false;
}

struct MainImageHeaders
{
    std::span<std::byte> first;

    template <class Value>
    bool Read(std::size_t offset, Value& value) const noexcept
    {
        if (offset > first.size() || first.size() - offset < sizeof(value)) return false;
        std::memcpy(&value, first.data() + offset, sizeof(value));
        return true;
    }

    bool ReadNtOffset(std::uint32_t& nt) const noexcept
    {
        std::uint16_t dos{};
        std::uint32_t signature{};
        return Read(0, dos) && dos == 0x5A4D && Read(0x3C, nt) &&
            Read(nt, signature) && signature == 0x4550;
    }

    bool ReadImageSize(std::uint32_t nt, std::uint32_t& imageSize) const noexcept
    {
        std::uint16_t optionalSize{};
        return Read(nt + 20, optionalSize) && optionalSize >= 64 &&
            Read(nt + 24 + 56, imageSize) && imageSize >= 4096 &&
            imageSize <= 0x80000000u;
    }
};

static bool MainImageRegionReady(const void* base, MEMORY_BASIC_INFORMATION& info) noexcept
{
    return QueryCommittedRegion(base, info) && info.RegionSize >= 0x1000 && RegionAccessible(info);
}

[[nodiscard]] std::span<std::byte> MainImage() noexcept
{
    const auto module = GetModuleHandleW(nullptr);
    if (module == nullptr) return {};
    auto* base = reinterpret_cast<std::byte*>(module);
    MEMORY_BASIC_INFORMATION info{};
    if (!MainImageRegionReady(base, info)) return {};
    const MainImageHeaders headers{std::span<std::byte>(base, info.RegionSize)};
    std::uint32_t nt{};
    std::uint32_t imageSize{};
    if (!headers.ReadNtOffset(nt)) return {};
    if (!headers.ReadImageSize(nt, imageSize)) return {};
    if (!IsCommittedAccessible(base, imageSize)) return {};
    return {base, imageSize};
}

namespace {
PointerHookOwnership g_d3dIat;
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_nextD3d{};

// Latched once per published device generation so every same-role texture of
// that generation gets one format, independent of later health changes.
struct ScenePromotionLatch
{
    bool eligible{};
    std::span<std::byte> gameImage;
};
ScenePromotionLatch g_scenePromotion;
}

template <class Record, std::size_t Count>
[[nodiscard]] Record* AllocateRecord(
    std::array<Record, Count>& records) noexcept
{
    for (auto& record : records)
        if (record.identity.Object() == nullptr) return &record;
    return nullptr;
}

[[nodiscard]] bool ValidateNext(void* next, void* hook) noexcept
{
    return next != nullptr && next != hook && IsExecutableAddress(next);
}

template <class Object, class Record>
[[nodiscard]] bool PublishObjectShadow(Object* object, Record& record, void** current) noexcept
{
    const auto generation = g_objectGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    if constexpr (std::is_same_v<Record, ContextRecord>) record.generation = generation;
    if (!record.identity.Publish(object, generation)) return false;
    if (!ObjectCompareExchange(reinterpret_cast<void**>(object), current, record.shadow.data()))
    {
        (void)record.identity.Retire(object, generation);
        return false;
    }
    return true;
}

[[nodiscard]] bool ValidateShadowPredecessors(void** current, std::span<const VtablePatch> patches) noexcept
{
    for (const auto& patch : patches)
        if (!ValidateNext(current[patch.index], patch.replacement)) return false;
    return true;
}

template <std::size_t Count>
[[nodiscard]] bool ValidateCapturedPredecessors(const std::array<void*, Count>& captured,
    const std::array<VtablePatch, Count>& patches) noexcept
{
    for (std::size_t index = 0; index < Count; ++index)
        if (!ValidateNext(captured[index], patches[index].replacement)) return false;
    return true;
}
[[nodiscard]] bool PrepareDeviceRecord(DeviceRecord* record,
    void** current, std::size_t slots) noexcept
{
    const std::array patches{
        VtablePatch{kRelease,
            reinterpret_cast<void*>(&HookDeviceRelease)},
        VtablePatch{kDeviceCreateTexture2D,
            reinterpret_cast<void*>(&HookCreateTexture2D)},
        VtablePatch{kDeviceCreateSrv,
            reinterpret_cast<void*>(&HookCreateSrv)},
        VtablePatch{kDeviceCreateUav,
            reinterpret_cast<void*>(&HookCreateUav)},
        VtablePatch{kDeviceCreateRtv,
            reinterpret_cast<void*>(&HookCreateRtv)},
        VtablePatch{kDeviceCreatePixelShader,
            reinterpret_cast<void*>(&HookCreatePixelShader)}};
    if (!PrepareShadowVtable(
            std::span<void* const>(current, slots),
            std::span<void*>(record->shadow.data(), slots), patches))
        return false;
    record->originalVtable = current;
    record->nextRelease =
        reinterpret_cast<DeviceRelease>(current[kRelease]);
    record->nextCreateTexture2D =
        reinterpret_cast<CreateTexture2D>(current[kDeviceCreateTexture2D]);
    record->nextCreateSrv = reinterpret_cast<CreateShaderResourceView>(
        current[kDeviceCreateSrv]);
    record->nextCreateUav = reinterpret_cast<CreateUnorderedAccessView>(
        current[kDeviceCreateUav]);
    record->nextCreateRtv = reinterpret_cast<CreateRenderTargetView>(
        current[kDeviceCreateRtv]);
    record->nextCreatePixelShader = reinterpret_cast<CreatePixelShader>(
        current[kDeviceCreatePixelShader]);
    const std::array captured{
        reinterpret_cast<void*>(record->nextRelease),
        reinterpret_cast<void*>(record->nextCreateTexture2D),
        reinterpret_cast<void*>(record->nextCreateSrv),
        reinterpret_cast<void*>(record->nextCreateUav),
        reinterpret_cast<void*>(record->nextCreateRtv),
        reinterpret_cast<void*>(record->nextCreatePixelShader)};
    return ValidateCapturedPredecessors(captured, patches);
}

[[nodiscard]] bool InstallDeviceShadow(ID3D11Device* device) noexcept
{
    if (device == nullptr) return false;
    BoundedAtomicFlagGuard guard(g_installLock, kHookRegistryAttempts);
    if (!guard.OwnsLock()) return false;
    if (FindDevice(device) != nullptr) return true;
    auto* record = AllocateRecord(g_devices);
    if (record == nullptr) return false;
    auto* current = *reinterpret_cast<void***>(device);
    const auto slots = DiscoverVtableSlots(current, kDeviceBaseSlots);
    if (slots == 0) return false;
    if (!PrepareDeviceRecord(record, current, slots)) return false;
    if (!PublishObjectShadow(device, *record, current)) return false;
    g_evidence.deviceShadowCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

[[nodiscard]] bool PrepareSharedUiContextHooks(ContextRecord* record, void** current) noexcept
{
    if (g_configuration.productionRequested)
    {
        const std::array patches{
            VtablePatch{kContextDispatch, reinterpret_cast<void*>(&HookDispatch)},
            VtablePatch{kContextDispatchIndirect, reinterpret_cast<void*>(&HookDispatchIndirect)},
            VtablePatch{kContextClearUnorderedAccessViewUint, reinterpret_cast<void*>(&HookClearUnorderedAccessViewUint)},
            VtablePatch{kContextClearUnorderedAccessViewFloat, reinterpret_cast<void*>(&HookClearUnorderedAccessViewFloat)}};
        if (!ValidateShadowPredecessors(current, patches)) return false;
        for (const auto& patch : patches) record->shadow[patch.index] = patch.replacement;
    }
    record->nextDispatch = reinterpret_cast<Dispatch>(current[kContextDispatch]);
    record->nextDispatchIndirect = reinterpret_cast<DispatchIndirect>(current[kContextDispatchIndirect]);
    record->nextClearUnorderedAccessViewUint =
        reinterpret_cast<ClearUnorderedAccessViewUint>(current[kContextClearUnorderedAccessViewUint]);
    record->nextClearUnorderedAccessViewFloat =
        reinterpret_cast<ClearUnorderedAccessViewFloat>(current[kContextClearUnorderedAccessViewFloat]);
    return true;
}

[[nodiscard]] bool PrepareContextRecord(ContextRecord* record,
    void** current, std::size_t slots) noexcept
{
    const std::array patches{
        VtablePatch{kRelease,
            reinterpret_cast<void*>(&HookContextRelease)},
        VtablePatch{kContextPsSetShader,
            reinterpret_cast<void*>(&HookPsSetShader)},
        VtablePatch{kContextDrawIndexed,
            reinterpret_cast<void*>(&HookDrawIndexed)},
        VtablePatch{kContextDraw, reinterpret_cast<void*>(&HookDraw)},
        VtablePatch{kContextDrawIndexedInstanced,
            reinterpret_cast<void*>(&HookDrawIndexedInstanced)},
        VtablePatch{kContextDrawInstanced,
            reinterpret_cast<void*>(&HookDrawInstanced)},
        VtablePatch{kContextBegin, reinterpret_cast<void*>(&HookBegin)},
        VtablePatch{kContextEnd, reinterpret_cast<void*>(&HookEnd)},
        VtablePatch{kContextExecuteCommandList,
            reinterpret_cast<void*>(&HookExecuteCommandList)},
        VtablePatch{kContextOmSetRenderTargets,
            reinterpret_cast<void*>(&HookOmSetRenderTargets)},
        VtablePatch{kContextOmSetRenderTargetsAndUavs,
            reinterpret_cast<void*>(&HookOmSetRenderTargetsAndUavs)}};
    if (!PrepareShadowVtable(
            std::span<void* const>(current, slots),
            std::span<void*>(record->shadow.data(), slots), patches))
        return false;
    if (FrameTraceRequested() || g_configuration.productionRequested)
    {
        const std::array mutationPatches{
            VtablePatch{kContextCopySubresourceRegion, reinterpret_cast<void*>(&HookCopySubresourceRegion)},
            VtablePatch{kContextCopyResource, reinterpret_cast<void*>(&HookCopyResource)},
            VtablePatch{kContextClearRenderTargetView, reinterpret_cast<void*>(&HookClearRenderTargetView)}};
        if (!ValidateShadowPredecessors(current, mutationPatches)) return false;
        for (const auto& patch : mutationPatches) record->shadow[patch.index] = patch.replacement;
    }
    if (!PrepareSharedUiContextHooks(record, current)) return false;
    record->originalVtable = current;
    record->nextRelease =
        reinterpret_cast<ContextRelease>(current[kRelease]);
    record->nextPsSetShader =
        reinterpret_cast<PsSetShader>(current[kContextPsSetShader]);
    record->nextDrawIndexed =
        reinterpret_cast<DrawIndexed>(current[kContextDrawIndexed]);
    record->nextDraw = reinterpret_cast<Draw>(current[kContextDraw]);
    record->nextDrawIndexedInstanced =
        reinterpret_cast<DrawIndexedInstanced>(
            current[kContextDrawIndexedInstanced]);
    record->nextDrawInstanced =
        reinterpret_cast<DrawInstanced>(current[kContextDrawInstanced]);
    record->nextBegin = reinterpret_cast<BeginQuery>(current[kContextBegin]);
    record->nextEnd = reinterpret_cast<EndQuery>(current[kContextEnd]);
    record->nextExecuteCommandList =
        reinterpret_cast<ExecuteCommandList>(current[kContextExecuteCommandList]);
    record->nextCopySubresourceRegion = reinterpret_cast<CopySubresourceRegion>(current[kContextCopySubresourceRegion]);
    record->nextCopyResource = reinterpret_cast<CopyResource>(current[kContextCopyResource]);
    record->nextClearRenderTargetView = reinterpret_cast<ClearRenderTargetView>(current[kContextClearRenderTargetView]);
    record->computeQueries = {};
    record->nextOmSetRenderTargets = reinterpret_cast<OmSetRenderTargets>(
        current[kContextOmSetRenderTargets]);
    record->nextOmSetRenderTargetsAndUavs =
        reinterpret_cast<OmSetRenderTargetsAndUavs>(
            current[kContextOmSetRenderTargetsAndUavs]);
    return ValidateShadowPredecessors(current, patches);
}

[[nodiscard]] bool InstallContextShadow(
    ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr) return false;
    BoundedAtomicFlagGuard guard(g_installLock, kHookRegistryAttempts);
    if (!guard.OwnsLock()) return false;
    if (FindContext(context) != nullptr) return true;
    auto* record = AllocateRecord(g_contexts);
    if (record == nullptr) return false;
    record->immediate = context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    auto* current = *reinterpret_cast<void***>(context);
    const auto slots = DiscoverVtableSlots(current, kContextBaseSlots);
    if (slots == 0) return false;
    if (!PrepareContextRecord(record, current, slots)) return false;
    return PublishObjectShadow(context, *record, current);
}

[[nodiscard]] bool PrepareSwapChainRecord(SwapChainRecord* record,
    void** current, std::size_t slots) noexcept
{
    const std::array patches{
        VtablePatch{kRelease,
            reinterpret_cast<void*>(&HookSwapChainRelease)},
        VtablePatch{kSwapChainPresent,
            reinterpret_cast<void*>(&HookPresent)},
        VtablePatch{kSwapChainResizeBuffers,
            reinterpret_cast<void*>(&HookResizeBuffers)}};
    if (!PrepareShadowVtable(
            std::span<void* const>(current, slots),
            std::span<void*>(record->shadow.data(), slots), patches))
        return false;
    record->originalVtable = current;
    record->nextRelease =
        reinterpret_cast<SwapChainRelease>(current[kRelease]);
    record->nextPresent = reinterpret_cast<Present>(current[kSwapChainPresent]);
    record->nextResizeBuffers =
        reinterpret_cast<ResizeBuffers>(current[kSwapChainResizeBuffers]);
    const std::array captured{
        reinterpret_cast<void*>(record->nextRelease),
        reinterpret_cast<void*>(record->nextPresent),
        reinterpret_cast<void*>(record->nextResizeBuffers)};
    return ValidateCapturedPredecessors(captured, patches);
}

[[nodiscard]] bool InstallSwapChainShadow(
    IDXGISwapChain* swapChain, bool hdrDescriptor,
    bool ownsRendererClaim) noexcept
{
    if (swapChain == nullptr) return false;
    BoundedAtomicFlagGuard guard(g_installLock, kHookRegistryAttempts);
    if (!guard.OwnsLock()) return false;
    if (auto* existing = FindSwapChain(swapChain))
    {
        existing->developerHdrDescriptor = hdrDescriptor;
        existing->ownsRendererClaim |= ownsRendererClaim;
        return true;
    }
    auto* record = AllocateRecord(g_swapChains);
    if (record == nullptr) return false;
    auto* current = *reinterpret_cast<void***>(swapChain);
    const auto slots = DiscoverVtableSlots(current, kSwapChainBaseSlots);
    if (slots == 0) return false;
    if (!PrepareSwapChainRecord(record, current, slots)) return false;
    record->developerHdrDescriptor = hdrDescriptor;
    record->applicationFp16Facade = false;
    record->ownsRendererClaim = ownsRendererClaim;
    return PublishObjectShadow(swapChain, *record, current);
}

template <class Record, std::size_t Count>
[[nodiscard]] void** ShadowOriginalTable(std::array<Record, Count>& records, void** table) noexcept
{
    const auto shadow = std::ranges::find_if(records, [table](const Record& record) {
        return record.originalVtable != nullptr && record.shadow.data() == table;
    });
    return shadow == records.end() ? nullptr : shadow->originalVtable;
}

// A rolled-back object already carries its original table again.
[[nodiscard]] void** ForwardingTable(void** table) noexcept
{
    for (auto* original : {ShadowOriginalTable(g_devices, table),
             ShadowOriginalTable(g_contexts, table), ShadowOriginalTable(g_swapChains, table)})
        if (original != nullptr) return original;
    return table;
}

void* ResolveUnrecordedNext(void* object, VtablePatch hook) noexcept
{
    if (object == nullptr || hook.index >= kMaximumVtableSlots) return nullptr;
    auto* table = static_cast<void**>(AtomicPointerLoad(static_cast<void* const*>(object)));
    if (table == nullptr) return nullptr;
    auto* const* slot = ForwardingTable(table) + hook.index;
    if (!IsReadable(slot, sizeof(void*))) return nullptr;
    auto* next = AtomicPointerLoad(slot);
    return ValidateNext(next, hook.replacement) ? next : nullptr;
}

template <class Object, class Record>
void RollBackShadow(Object* object, Record* record) noexcept
{
    if (object == nullptr || record == nullptr) return;
    if (record->originalVtable == nullptr)
        return;
    const auto generation = record->identity.Generation();
    if (ObjectCompareExchange(reinterpret_cast<void**>(object),
            record->shadow.data(), record->originalVtable))
        (void)record->identity.Retire(object, generation);
}

void ClearCreationOutputs(
    IDXGISwapChain** swapChain, ID3D11Device** device,
    ID3D11DeviceContext** context) noexcept
{
    if (context != nullptr && *context != nullptr)
    {
        (*context)->Release();
        *context = nullptr;
    }
    if (device != nullptr && *device != nullptr)
    {
        (*device)->Release();
        *device = nullptr;
    }
    if (swapChain != nullptr && *swapChain != nullptr)
    {
        (*swapChain)->Release();
        *swapChain = nullptr;
    }
}

[[nodiscard]] bool ValidCreatedHdrDescriptor(IDXGISwapChain* swapChain) noexcept
{
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description))) return false;
    return HdrFlipDescriptorSupported(description) && description.SampleDesc.Count == 1 &&
        (description.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) != 0;
}

struct HdrCandidateValidation
{
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> outputRtv;
    ComPtr<IDXGISwapChain3> chain3;

    [[nodiscard]] bool SupportsRenderTarget(ID3D11Device* device) noexcept
    {
        UINT support{};
        return SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support)) &&
            (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;
    }

    [[nodiscard]] bool AcquireOutput(IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
    {
        return SupportsRenderTarget(device) && SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) &&
            SUCCEEDED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &outputRtv));
    }

    [[nodiscard]] bool SupportsColor(DXGI_COLOR_SPACE_TYPE color) noexcept
    {
        UINT support{};
        return SUCCEEDED(chain3->CheckColorSpaceSupport(color, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
    }

    [[nodiscard]] bool ValidateDisplay(IDXGISwapChain* swapChain) noexcept
    {
        if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3)))) return false;
        if (!SupportsColor(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)) return false;
        DXGI_SWAP_CHAIN_DESC description{};
        return SUCCEEDED(swapChain->GetDesc(&description)) &&
            RefreshDisplayCapabilities(description.OutputWindow) &&
            CurrentDisplayCapabilities().description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }

    [[nodiscard]] bool ValidateTransitions() noexcept
    {
        return SupportsColor(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) &&
            SUCCEEDED(chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)) &&
            SUCCEEDED(chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    }
};

[[nodiscard]] bool ValidateCreatedHdrCandidate(IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
{
    if (swapChain == nullptr || device == nullptr) return false;
    if (!ValidCreatedHdrDescriptor(swapChain)) return false;
    HdrCandidateValidation validation;
    return validation.AcquireOutput(swapChain, device) && validation.ValidateDisplay(swapChain) &&
        validation.ValidateTransitions();
}

struct D3dCreationRequest
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

    [[nodiscard]] HRESULT Invoke(const DXGI_SWAP_CHAIN_DESC* selected) const
    {
        g_evidence.deviceNextCallCount.fetch_add(1, std::memory_order_relaxed);
        return InvokeCapturedNext(g_nextD3d, adapter, driverType, software, flags,
            levels, levelCount, sdkVersion, selected, swapChain, device, featureLevel, context);
    }

    void ClearOutputs() const noexcept { ClearCreationOutputs(swapChain, device, context); }
};

template <class Object>
[[nodiscard]] bool HasCreatedObject(Object** output) noexcept
{
    return output != nullptr && *output != nullptr;
}

struct InstalledRendererObjects
{
    bool device{};
    bool context{};
    bool chain{};
    bool proxy{};
    bool facade{};
    [[nodiscard]] bool Complete() const noexcept { return device && context && chain && proxy && facade; }
};

void RetireApplicationPresentationBuffer(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr) return;
    if (swapChain != g_presentation.applicationFacadeChain) return;
    // Release only owned references before the facade flushes its retired texture.
    ResetResources(true);
    g_referenceHealthy = false;
}

class RendererCreationTransaction
{
public:
    explicit RendererCreationTransaction(const D3dCreationRequest& request) : request_(request) {}

    [[nodiscard]] HRESULT Run()
    {
        g_evidence.deviceCreateCount.fetch_add(1, std::memory_order_relaxed);
        owner_ = ClaimRenderer();
        attemptHdr_ = owner_ && HdrPathRequested();
        if (attemptHdr_) CreateHdrCandidate();
        else result_ = request_.Invoke(request_.description);
        g_evidence.lastDeviceResult.store(result_, std::memory_order_release);
        if (FAILED(result_))
        {
            ReleaseFailedCreationClaim();
            return result_;
        }
        g_evidence.successfulDeviceCreateCount.fetch_add(1, std::memory_order_relaxed);
        // Secondary creations must not publish into the presenting renderer.
        if (!owner_) return result_;
        InitializeRendererLifetime();
        RetirePreviousDevice();
        const auto installed = InstallObjects();
        if (installed.Complete()) PublishInstallation();
        ReportInstallationFailure(installed);
        if (!installed.Complete()) RollBackInstallation(installed);
        // Descriptor ownership only initializes the chain. Present publishes
        // HDR after ordered heartbeat, composition, color-space and output checks.
        g_evidence.hdrChainActive.store(false, std::memory_order_release);
        return result_;
    }

private:    void ReleaseFailedCreationClaim()
    {
        if (owner_ && !attemptHdr_)
            g_rendererCreationClaimed.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool ClaimRenderer()
    {
        if (request_.description == nullptr || request_.description->OutputWindow == nullptr) return false;
        bool expected{};
        return g_rendererCreationClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool CandidateUsable()
    {
        return SUCCEEDED(result_) && HasCreatedObject(request_.swapChain) &&
            HasCreatedObject(request_.device) &&
            ValidateCreatedHdrCandidate(*request_.swapChain, *request_.device) &&
            BuildReplacementShaders(*request_.device);
    }

    void RetryOriginalDescriptor()
    {
        g_evidence.hdrCreationFallbackCount.fetch_add(1, std::memory_order_relaxed);
        result_ = request_.Invoke(request_.description);
        if (SUCCEEDED(result_))
            g_evidence.hdrCreationFallbackSuccessCount.fetch_add(1, std::memory_order_relaxed);
    }

    void CreateHdrCandidate()
    {
        g_evidence.hdrCreationAttemptCount.fetch_add(1, std::memory_order_relaxed);
        auto modified = *request_.description;
        modified.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        modified.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
        modified.BufferCount = std::max<UINT>(2, modified.BufferCount);
        modified.SampleDesc.Count = 1;
        modified.SampleDesc.Quality = 0;
        modified.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (!g_configuration.forceHdrSwapChainFailure) result_ = request_.Invoke(&modified);
        if (!CandidateUsable())
        {
            if (SUCCEEDED(result_)) RecordFailure(GraphicsObservationFailure::hdrAttemptFailed);
            ResetReplacementShaders();
            request_.ClearOutputs();
            RetryOriginalDescriptor();
            g_referenceHealthy = false;
            return;
        }
        hdrDescriptor_ = true;
        g_referenceHealthy.store(true, std::memory_order_release);
        g_productionHealthy.store(true, std::memory_order_release);
        PublishActiveDisplay(CurrentDisplayCapabilities().description.Monitor, 1);
    }

    void ReadCreatedDimensions()
    {
        DXGI_SWAP_CHAIN_DESC actual{};
        if (HasCreatedObject(request_.swapChain) && SUCCEEDED((*request_.swapChain)->GetDesc(&actual)))
        {
            g_presentation.width = actual.BufferDesc.Width;
            g_presentation.height = actual.BufferDesc.Height;
            g_outputWindow = actual.OutputWindow;
        }
    }

    void InitializeRendererLifetime()
    {
        ReadCreatedDimensions();
        if (g_lifecycle.Snapshot().state == HdrRendererState::deviceLost)
            (void)g_lifecycle.BindDeviceAndSwapChain();
        else if (g_lifecycle.Snapshot().state == HdrRendererState::detached)
            (void)g_lifecycle.BindDeviceAndSwapChain();
        const auto generation = PublishLifecycleGeneration();
        g_heartbeat.Reset(generation);
        g_frame = 1;
        g_heartbeat.BeginFrame(generation, g_frame);
        g_currentFrameQualifiedDrawThread.store(0, std::memory_order_release);
        g_currentFrameQualifiedDrawThreadConflict.store(false, std::memory_order_release);
        g_observedQualifiedDrawThread.store(0, std::memory_order_release);
        g_observedPresentThread.store(0, std::memory_order_release);
        g_liveSettingsThreadCompatible.store(true, std::memory_order_release);
    }

    void RetirePreviousDevice()
    {
        if (g_rendererDevice.activeDevice == nullptr || !HasCreatedObject(request_.device)) return;
        if (g_rendererDevice.activeDevice == *request_.device) return;
        ShutdownSettingsWindowBackend();
        if (auto* record = FindDrawObservation(g_rendererDevice.activeContext)) record->nativePixelShader.Reset();
        g_presentation.applicationFacadeChain = nullptr;
        ResetResources();
        ResetShaderRegistry();
    }

    [[nodiscard]] InstalledRendererObjects InstallObjects()
    {
        InstalledRendererObjects installed;
        installed.device = HasCreatedObject(request_.device) && InstallDeviceShadow(*request_.device);
        installed.context = installed.device && HasCreatedObject(request_.context) &&
            InstallContextShadow(*request_.context);
        installed.chain = installed.context && HasCreatedObject(request_.swapChain) &&
            InstallSwapChainShadow(*request_.swapChain, hdrDescriptor_, owner_);
        installed.proxy = installed.chain && CreateHdrResources();
        installed.facade = installed.proxy && CreateApplicationFacade();
        return installed;
    }

    [[nodiscard]] bool CreateHdrResources()
    {
        return !hdrDescriptor_ ||
            EnsureWideGamutResources(*request_.swapChain, *request_.device, *request_.context);
    }

    [[nodiscard]] bool CreateApplicationFacade()
    {
        if (!g_configuration.productionRequested || !hdrDescriptor_) return true;
        if (!InitializeSharedUiSdr(*request_.context)) return false;
        return SUCCEEDED(CreatePresentationSwapChain({*request_.swapChain,
            g_presentation.presentationProxy.Get(), *request_.description,
            AcquireApplicationPresentationBuffer, RetireApplicationPresentationBuffer}, &facade_));
    }

    void PublishInstallation()
    {
        g_rendererDevice.activeDevice = *request_.device;
        g_rendererDevice.activeContext = *request_.context;
        g_rendererDevice.activeContext1.Reset();
        (void)(*request_.context)->QueryInterface(IID_PPV_ARGS(&g_rendererDevice.activeContext1));
        ++g_deviceGeneration;
        g_scenePromotion = {HdrPathRequested() && hdrDescriptor_ && g_referenceHealthy && g_productionHealthy,
            MainImage()};
        if (SettingsWindowBackendReady()) (void)TryActivateGraphicsSettingsInputOnCurrentThread();
        if (facade_)
        {
            auto* nativeChain = *request_.swapChain;
            g_presentation.applicationFacadeChain = nativeChain;
            FindSwapChain(nativeChain)->applicationFp16Facade = true;
            *request_.swapChain = facade_.Detach();
            nativeChain->Release();
        }
    }

    static void ReportInstallationFailure(InstalledRendererObjects installed)
    {
        if (!installed.device)
        {
            g_evidence.deviceShadowInstallFailureCount.fetch_add(1, std::memory_order_relaxed);
            RecordFailure(GraphicsObservationFailure::deviceShadowFailed);
        }
        else if (!installed.context) RecordFailure(GraphicsObservationFailure::contextShadowFailed);
        else if (!installed.chain) RecordFailure(GraphicsObservationFailure::swapChainShadowFailed);
        else if (!installed.proxy) RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
        else if (!installed.facade) RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
    }

    void RollBackInstallation(InstalledRendererObjects installed)
    {
        facade_.Reset();
        g_presentation.applicationFacadeChain = nullptr;
        if (installed.chain) RollBackShadow(*request_.swapChain, FindSwapChain(*request_.swapChain));
        if (installed.context) RollBackShadow(*request_.context, FindContext(*request_.context));
        if (installed.device) RollBackShadow(*request_.device, FindDevice(*request_.device));
        ShutdownSettingsWindowBackend();
        ResetResources();
        ResetReplacementShaders();
        g_referenceHealthy = false;
        g_rendererDevice.activeDevice = nullptr;
        g_rendererDevice.activeContext = nullptr;
        g_rendererDevice.activeContext1.Reset();
        g_presentation.width = 0;
        g_presentation.height = 0;
        g_outputWindow = nullptr;
        g_lifecycle.Detach();
        if (hdrDescriptor_)
        {
            request_.ClearOutputs();
            RetryOriginalDescriptor();
            g_evidence.lastDeviceResult.store(result_, std::memory_order_release);
        }
    }

    const D3dCreationRequest& request_;
    bool owner_{};
    bool attemptHdr_{};
    bool hdrDescriptor_{};
    ComPtr<IDXGISwapChain> facade_;
    HRESULT result_{E_FAIL};
};


HRESULT WINAPI HookD3dCreate(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
    UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* description,
    IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** context) noexcept
{
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        const D3dCreationRequest request{adapter, driverType, software, flags, levels,
            levelCount, sdkVersion, description, swapChain, device, featureLevel, context};
        return RendererCreationTransaction(request).Run();
    }, []() noexcept { RecordAbiException(); });
}

[[nodiscard]] bool PublishD3dNext(void* next) noexcept
{
    const auto function =
        reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(next);
    if (!ValidateNext(next, reinterpret_cast<void*>(&HookD3dCreate)))
    {
        RecordFailure(GraphicsObservationFailure::selfReferentialNext);
        return false;
    }
    g_nextD3d = function;
    return true;
}

[[nodiscard]] GraphicsObservationFailure InstallGraphicsImport() noexcept
{
    const auto image = MainImage();
    if (image.empty()) return GraphicsObservationFailure::imageUnavailable;
    const auto d3d = FindPe32PlusIatSlot(image, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    if (d3d.status != PeIatStatus::success) return GraphicsObservationFailure::importUnavailable;
    if (!g_d3dIat.Install(d3d.slot, reinterpret_cast<void*>(&HookD3dCreate),
            ProtectedCompareExchange, PublishD3dNext))
        return GraphicsObservationFailure::iatInstallFailed;
    return GraphicsObservationFailure::none;
}

HookOwnershipState GraphicsImportHealth() noexcept
{
    return g_d3dIat.Health();
}

ULONG STDMETHODCALLTYPE HookDeviceRelease(ID3D11Device* device) noexcept
{
    return InvokeNoexceptBoundary<ULONG>(1, [=]() {
        auto* record = FindDevice(device);
        if (record == nullptr || record->nextRelease == nullptr)
            return ForwardUnrecorded<DeviceRelease>(1UL, HookAt(kRelease, &HookDeviceRelease),
                device);
        const auto generation = record->identity.Generation();
        const auto remaining = InvokeCapturedNext(record->nextRelease, device);
        if (remaining == 0 && record->identity.Retire(device, generation))
            g_evidence.deviceShadowRetireCount.fetch_add(
                1, std::memory_order_relaxed);
        return remaining;
    }, []() noexcept { RecordAbiException(); });
}

[[nodiscard]] DXGI_FORMAT RemapPromotedView(
    ID3D11Resource* resource, DXGI_FORMAT requested) noexcept
{
    if (resource == nullptr || requested != DXGI_FORMAT_R11G11B10_FLOAT)
        return requested;
    std::uint8_t tag{};
    UINT size = sizeof(tag);
    const auto promoted = SUCCEEDED(resource->GetPrivateData(kPromotedSceneTexture, &size, &tag)) &&
        size == sizeof(tag);
    return promoted ? DXGI_FORMAT_R16G16B16A16_FLOAT : requested;
}

// The shadow vtable sits directly under the game's call site, so the game's
// own targets return into its image; other plugins sharing the device do not.
bool GameScenePromotion(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initialData, const void* caller) noexcept
{
    return device == g_rendererDevice.activeDevice && description != nullptr && g_scenePromotion.eligible &&
        ImageContains(g_scenePromotion.gameImage, caller) &&
        IsSceneTexturePromotionCandidate(*description, initialData != nullptr,
            g_presentation.width, g_presentation.height);
}
void TagPromotedTexture(HRESULT result, ID3D11Texture2D** texture) noexcept
{
    if (FAILED(result) || !HasCreatedObject(texture)) return;
    g_evidence.promotedResourceCount.fetch_add(1, std::memory_order_relaxed);
    // Fails only on E_OUTOFMEMORY; the texture still forwards exactly once.
    constexpr std::uint8_t tag{1};
    if (FAILED((*texture)->SetPrivateData(kPromotedSceneTexture, sizeof(tag), &tag)))
        RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
}
HRESULT STDMETHODCALLTYPE HookCreateTexture2D(
    ID3D11Device* device, const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initialData,
    ID3D11Texture2D** texture) noexcept
{
    const void* caller = _ReturnAddress();
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        auto* record = FindDevice(device);
        if (record == nullptr || record->nextCreateTexture2D == nullptr)
            return ForwardUnrecorded<CreateTexture2D>(E_FAIL,
                HookAt(kDeviceCreateTexture2D, &HookCreateTexture2D),
                device, description, initialData, texture);
        if (!GameScenePromotion(device, description, initialData, caller))
            return InvokeCapturedNext(record->nextCreateTexture2D, device,
                description, initialData, texture);
        auto promoted = *description;
        promoted.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto result = InvokeCapturedNext(record->nextCreateTexture2D,
            device, &promoted, initialData, texture);
        TagPromotedTexture(result, texture);
        return result;
    }, []() noexcept { RecordAbiException(); });
}

template <auto NextMember, std::size_t Slot, auto Hook, class Description, class View>
HRESULT CreatePromotedView(ID3D11Device* device, ID3D11Resource* resource,
    const Description* description, View** view) noexcept
{
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        auto* record = FindDevice(device);
        if (record == nullptr || record->*NextMember == nullptr)
            return ForwardUnrecorded<std::remove_cvref_t<decltype(record->*NextMember)>>(
                E_FAIL, HookAt(Slot, Hook), device, resource, description, view);
        if (device != g_rendererDevice.activeDevice || description == nullptr)
            return InvokeCapturedNext(record->*NextMember, device, resource, description, view);
        auto remapped = *description;
        remapped.Format = RemapPromotedView(resource, remapped.Format);
        return InvokeCapturedNext(record->*NextMember, device, resource, &remapped, view);
    }, []() noexcept { RecordAbiException(); });
}

HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description, ID3D11ShaderResourceView** view) noexcept
{
    return CreatePromotedView<&DeviceRecord::nextCreateSrv, kDeviceCreateSrv, &HookCreateSrv>(
        device, resource, description, view);
}

HRESULT STDMETHODCALLTYPE HookCreateUav(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* description, ID3D11UnorderedAccessView** view) noexcept
{
    return CreatePromotedView<&DeviceRecord::nextCreateUav, kDeviceCreateUav, &HookCreateUav>(
        device, resource, description, view);
}
HRESULT STDMETHODCALLTYPE HookCreateRtv(
    ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* description,
    ID3D11RenderTargetView** view) noexcept
{
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        auto* record = FindDevice(device);
        if (record == nullptr || record->nextCreateRtv == nullptr)
            return ForwardUnrecorded<CreateRenderTargetView>(E_FAIL,
                HookAt(kDeviceCreateRtv, &HookCreateRtv),
                device, resource, description, view);
        if (device != g_rendererDevice.activeDevice)
            return InvokeCapturedNext(record->nextCreateRtv, device,
                resource, description, view);
        D3D11_RENDER_TARGET_VIEW_DESC remapped{};
        const D3D11_RENDER_TARGET_VIEW_DESC* selected = description;
        if (description != nullptr)
        {
            remapped = *description;
            remapped.Format = RemapPromotedView(resource, remapped.Format);
            selected = &remapped;
        }
        return InvokeCapturedNext(record->nextCreateRtv,
            device, resource, selected, view);
    }, []() noexcept { RecordAbiException(); });
}



HRESULT STDMETHODCALLTYPE HookCreatePixelShader(
    ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept
{
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        auto* record = FindDevice(device);
        if (record == nullptr || record->nextCreatePixelShader == nullptr)
            return ForwardUnrecorded<CreatePixelShader>(E_FAIL,
                HookAt(kDeviceCreatePixelShader, &HookCreatePixelShader),
                device, bytecode, bytecodeLength, linkage, shader);
        if (device != g_rendererDevice.activeDevice)
            return InvokeCapturedNext(record->nextCreatePixelShader, device,
                bytecode, bytecodeLength, linkage, shader);
        g_evidence.pixelShaderCreateCount.fetch_add(
            1, std::memory_order_relaxed);

        CreatedShaderIdentity identity;
        identity.Observe(bytecode, static_cast<std::size_t>(bytecodeLength), g_configuration.developerMode);

        // Always create and return the game's original shader exactly once.
        // Replacement shaders are pre-created transactionally before the
        // device shadow is installed, so fallback can bind this original
        // object without recreating it or forwarding twice.
        const auto result = InvokeCapturedNext(record->nextCreatePixelShader,
            device, bytecode, bytecodeLength, linkage, shader);
        g_evidence.lastPixelShaderResult.store(result,
            std::memory_order_release);
        identity.Register(result, shader);
        return result;
    }, []() noexcept { RecordAbiException(); });
}

} // namespace fo4hdr
