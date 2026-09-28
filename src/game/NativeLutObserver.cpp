// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "NativeLutObserver.hpp"
#include "ImageSpaceObserver.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/NativeLutObservation.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#if defined(FO4HDR_RUNTIME_1_10_163)
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#endif

namespace {
using Render = void (*)(RE::ImageSpaceEffect*, RE::BSTriShape*, RE::ImageSpaceEffectParam*);
std::atomic<bool> g_attempted{};
std::atomic<Render> g_next{};
RE::ImageSpaceManager** g_manager{};
std::uintptr_t g_vtable{};
static_assert(offsetof(RE::ImageSpaceManager, lutData) == 0x130);
static_assert(offsetof(RE::ImageSpaceManager, overrideLUTData) == 0x1A0);
static_assert(offsetof(RE::ImageSpaceLUTData, texture) == 0x50);
static_assert(offsetof(RE::NiTexture, name) == 0x10);
static_assert(offsetof(RE::NiTexture, rendererTexture) == 0x38);

bool ReadableRegion(const MEMORY_BASIC_INFORMATION& info) noexcept
{
    return info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

bool ContainsBytes(std::uintptr_t base, std::size_t extent, std::uintptr_t address, std::size_t size) noexcept
{
    return address >= base && address - base < extent && size <= extent - (address - base);
}
bool Region(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& info) noexcept
{
    if (!address || !size) return false;
    if (!::VirtualQuery(address, &info, sizeof(info))) return false;
    if (!ReadableRegion(info)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return ContainsBytes(base, info.RegionSize, start, size);
}

bool Read(const void* source, void* destination, std::size_t size) noexcept
{
    SIZE_T copied{};
    return source && ::ReadProcessMemory(::GetCurrentProcess(), source, destination,
        size, &copied) && copied == size;
}

// The draw owns the texture lifetime. Copy strings immediately and require both
// native representations to agree; a pending asset replacement remains global.
bool CopyPath(const char* source, std::array<char, 260>& result,
    std::uint16_t& length) noexcept
{
    std::array<char, 260> raw{};
    if (!source) return false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = source[i];
        if (raw[i] == '\0') {
            length = static_cast<std::uint16_t>(i);
            return fo4hdr::NormalizeLutGradePath({raw.data(), i}, result);
        }
    }
    return false;
}

bool ValidTexture(RE::BSGraphics::Texture* texture, RE::NiTexture* native, float weight) noexcept
{
    return texture && native && native->rendererTexture == texture &&
        std::isfinite(weight) && weight >= 0;
}
struct SlotIdentity
{
    RE::BSGraphics::Texture* texture;
    RE::NiTexture* native;
    const char* name;
    float weight;

    bool Unchanged(const RE::ImageSpaceLUTData& data, std::size_t i) const noexcept
    {
        return data.texture[i] == texture && data.niTexture[i].get() == native &&
            data.filename[i].c_str() == name && data.weight[i] == weight &&
            native->rendererTexture == texture;
    }
};
bool LoadedPathMatches(const RE::NiTexture& native, const fo4hdr::LutSnapshot& lut) noexcept
{
    std::array<char, 260> loadedName{};
    std::uint16_t loadedLength{};
    return CopyPath(native.name.c_str(), loadedName, loadedLength) && lut.utf8Path == loadedName;
}
std::uintptr_t TextureView(const RE::BSGraphics::Texture* texture) noexcept
{
#if defined(FO4HDR_RUNTIME_1_10_163)
    return *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const std::byte*>(texture) + fo4hdr::generated::kNativeLutTextureSrvOffset);
#else
    return 0;
#endif
}

bool CopyObservedPath(const char* name, fo4hdr::LutSnapshot& lut) noexcept
{
    const auto copied = CopyPath(name, lut.utf8Path, lut.pathLength);
    if (!copied) { lut.utf8Path = {}; lut.pathLength = 0; }
    return copied;
}

void PublishSlot(const SlotIdentity& identity, std::uintptr_t view,
    std::size_t index, fo4hdr::NativeLutObservation& result) noexcept
{
    result.luts[index].weight = identity.weight;
    result.luts[index].engineTextureId = reinterpret_cast<std::uintptr_t>(identity.texture);
    result.expectedShaderViews[index] = view;
    result.validSlots |= 1U << index;
}
void ObserveSlot(const RE::ImageSpaceLUTData* data, std::size_t i,
    fo4hdr::NativeLutObservation& result) noexcept
{
    auto* texture = data->texture[i];
    auto* native = data->niTexture[i].get();
    const auto* name = data->filename[i].c_str();
    const auto weight = data->weight[i];
    auto& lut = result.luts[i];
    const auto hasPath = CopyObservedPath(name, lut);
    lut.weight = weight;
    if (!ValidTexture(texture, native, weight))
        return;
    if (!hasPath || !LoadedPathMatches(*native, lut)) return;
    const auto view = TextureView(texture);
    if (!view) return;
    const SlotIdentity identity{texture, native, name, weight};
    if (!identity.Unchanged(*data, i)) return;
    PublishSlot(identity, view, i, result);
}

const RE::ImageSpaceLUTData* ActiveLut(const RE::ImageSpaceManager& manager) noexcept
{
    return manager.overrideLUTData ? manager.overrideLUTData : &manager.lutData;
}
bool Observe(RE::ImageSpaceEffect* effect, fo4hdr::NativeLutObservation& result) noexcept
{
    __try {
        if (!effect || *reinterpret_cast<std::uintptr_t*>(effect) != g_vtable) return false;
        auto* manager = *g_manager;
        if (!manager) return false;
        const auto* data = ActiveLut(*manager);
        for (std::size_t i = 0; i < result.luts.size(); ++i) {
            ObserveSlot(data, i, result);
        }
        if ((ActiveLut(*manager)) != data)
            return false;
        fo4hdr::game::AttachImageSpaceObservation(manager, data, result);
        result.threadId = ::GetCurrentThreadId();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RenderHook(RE::ImageSpaceEffect* effect, RE::BSTriShape* geometry,
    RE::ImageSpaceEffectParam* parameters) noexcept
{
    const auto next = g_next.load(std::memory_order_acquire);
    if (!next || next == &RenderHook) return;
    try {
        fo4hdr::NativeLutObservation observation{};
        if (fo4hdr::GetNativeLutObserverStatus() == fo4hdr::NativeLutObserverStatus::ready &&
            !Observe(effect, observation)) observation = {};
        fo4hdr::NativeLutScope scope(observation);
        next(effect, geometry, parameters);
    } catch (...) {
        fo4hdr::SetNativeLutObserverStatus(fo4hdr::NativeLutObserverStatus::unavailable);
    }
}
#if defined(FO4HDR_RUNTIME_1_10_163)
std::uintptr_t Resolve(const REL::Offset2ID& mappings, std::uintptr_t base, std::uint64_t id)
{
    for (const auto& entry : mappings)
        if (entry.id == id) return base + entry.offset;
    return 0;
}
bool InSection(const REX::FModule& module, const char* name,
    std::uintptr_t address, std::size_t size)
{
    const auto section = module.GetSection(name);
    if (!ContainsBytes(section.GetAddress(), section.GetSize(), address, size)) return false;
    MEMORY_BASIC_INFORMATION info{};
    return Region(reinterpret_cast<void*>(address), size, info) &&
        info.AllocationBase == reinterpret_cast<void*>(module.GetBaseAddress()) && info.Type == MEM_IMAGE;
}

bool ValidRenderTarget(const REX::FModule& module, std::uintptr_t target)
{
    using namespace fo4hdr::generated;
    std::array<std::uint8_t, kNativeLutRenderBytes.size()> bytes{};
    if (target != module.GetBaseAddress() + kNativeLutRenderRva) return false;
    if (!InSection(module, ".text", target, bytes.size())) return false;
    MEMORY_BASIC_INFORMATION codeInfo{};
    if (!Region(reinterpret_cast<void*>(target), bytes.size(), codeInfo)) return false;
    if ((codeInfo.Protect & 0xFF) != PAGE_EXECUTE_READ) return false;
    return Read(reinterpret_cast<void*>(target), bytes.data(), bytes.size()) && bytes == kNativeLutRenderBytes;
}

bool ValidLocator(const REX::FModule& module, std::uintptr_t table)
{
    std::uintptr_t locator{};
    std::uint32_t subobjectOffset{};
    if (!Read(reinterpret_cast<void*>(table - sizeof(void*)), &locator, sizeof(locator))) return false;
    if (!InSection(module, ".rdata", locator, 12)) return false;
    return Read(reinterpret_cast<void*>(locator + 4), &subobjectOffset, sizeof(subobjectOffset)) &&
        subobjectOffset == fo4hdr::generated::kNativeLutEffectSubobjectOffset;
}

bool ReadRenderSlot(void** slot, void*& original)
{
    MEMORY_BASIC_INFORMATION slotInfo{};
    if (!Region(slot, sizeof(void*), slotInfo)) return false;
    if (reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) != 0) return false;
    return Read(slot, &original, sizeof(original));
}
void RestoreRenderSlot(void** slot, void* original, bool replaced)
{
    DWORD writablePrior{};
    if (replaced && ::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &writablePrior))
        ::InterlockedCompareExchangePointer(slot, original, reinterpret_cast<void*>(&RenderHook));
}

bool InstallRenderSlot(void** slot, void* original)
{
    using fo4hdr::SetNativeLutObserverStatus;
    using Status = fo4hdr::NativeLutObserverStatus;
    DWORD prior{};
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior)) return false;
    // F4SE startup precedes render dispatch. The process-lifetime callback
    // remains callable on rollback failure; never overwrite a foreign owner.
    const auto replaced = ::InterlockedCompareExchangePointer(slot,
        reinterpret_cast<void*>(&RenderHook), original) == original;
    DWORD ignored{};
    const auto restored = ::VirtualProtect(slot, sizeof(void*), prior, &ignored) != FALSE;
    if (!replaced || !restored) {
        RestoreRenderSlot(slot, original, replaced);
        if (!restored) ::VirtualProtect(slot, sizeof(void*), prior, &ignored);
        SetNativeLutObserverStatus(replaced ? Status::invalidTarget : Status::conflictingHook);
        return false;
    }

    return true;
}
bool InstallQualifiedTarget(std::uintptr_t table, std::uintptr_t manager, std::uintptr_t target)
{
    using fo4hdr::SetNativeLutObserverStatus;
    using Status = fo4hdr::NativeLutObserverStatus;
    auto* slot = reinterpret_cast<void**>(table + sizeof(void*));
    void* original{};
    if (!ReadRenderSlot(slot, original)) return false;
    if (original != reinterpret_cast<void*>(target) || original == reinterpret_cast<void*>(&RenderHook)) {
        SetNativeLutObserverStatus(Status::conflictingHook);
        return false;
    }
    g_manager = reinterpret_cast<RE::ImageSpaceManager**>(manager);
    g_vtable = table;
    g_next.store(reinterpret_cast<Render>(target), std::memory_order_release);
    if (!InstallRenderSlot(slot, original)) return false;
    SetNativeLutObserverStatus(Status::ready);
    return true;
}
bool ResolveAndInstall(const REX::FModule& module)
{
    using namespace fo4hdr::generated;
    const auto base = module.GetBaseAddress();
    REL::Offset2ID mappings;
    mappings.load_v2();
    const auto table = Resolve(mappings, base, RE::VTABLE::BSImagespaceShaderGammaCorrectLUT[2].id());
    const auto manager = RE::ID::ImageSpaceManager::Singleton.address();
    const auto target = Resolve(mappings, base, kNativeLutRenderId);
    if (!InSection(module, ".rdata", table - sizeof(void*), 3 * sizeof(void*))) return false;
    if (!InSection(module, ".data", manager, sizeof(void*))) return false;
    if (!ValidRenderTarget(module, target)) return false;
    if (!ValidLocator(module, table)) return false;
    return InstallQualifiedTarget(table, manager, target);
}
#endif
}

namespace fo4hdr::game {
bool InitializeNativeLutObserver(bool requested) noexcept
{
    using Status = NativeLutObserverStatus;
    if (g_attempted.exchange(true)) return GetNativeLutObserverStatus() == Status::ready;
    if (!requested) return false;
    SetNativeLutObserverStatus(Status::unsupportedRuntime);
#if !defined(FO4HDR_RUNTIME_1_10_163)
    return false;
#else
    try {
        using namespace generated;
        const auto module = REX::FModule::GetExecutingModule();
        const auto base = module.GetBaseAddress();
        if (!base || module.GetFileVersion() != REL::Version{1, 10, 163, 0}) return false;
        SetNativeLutObserverStatus(Status::invalidTarget);
        return ResolveAndInstall(module);
    } catch (...) { return false; }
#endif
}
}
