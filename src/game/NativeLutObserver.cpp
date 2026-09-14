// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeLutObserver.hpp"
#include "GameObservationMemory.hpp"
#include "ImageSpaceObserver.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include <atomic>
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstring>

namespace {
using namespace fo4hdr::game::observation;
using LutBytes = std::array<std::byte, sizeof(RE::ImageSpaceLUTData)>;
static_assert(sizeof(RE::ImageSpaceLUTData) == 0x70);
static_assert(offsetof(RE::ImageSpaceLUTData, texture) == 0x50);
static_assert(offsetof(RE::NiTexture, name) == 0x10);
static_assert(offsetof(RE::NiTexture, rendererTexture) == 0x38);

bool CopyPath(const char* text, fo4hdr::LutSnapshot& destination) noexcept
{
    std::array<char, 260> bytes{};
    if (!ReadBoundedText(text, bytes)) return false;
    const std::string_view path(bytes.data());
    if (!fo4hdr::game::NormalizeObservedLutPath(path, destination.utf8Path)) return false;
    destination.pathLength = static_cast<std::uint16_t>(path.size());
    return true;
}

struct Slot
{
    std::uintptr_t texture{}, native{}, name{};
    float weight{};
};

bool ReadSlot(const LutBytes& bytes, std::size_t index, Slot& destination) noexcept
{
    return ReadSnapshotField(bytes, offsetof(RE::ImageSpaceLUTData, filename) + index * 8,
               destination.name) &&
        ReadSnapshotField(bytes, offsetof(RE::ImageSpaceLUTData, weight) + index * 4,
            destination.weight) &&
        ReadSnapshotField(bytes, offsetof(RE::ImageSpaceLUTData, niTexture) + index * 8,
            destination.native) &&
        ReadSnapshotField(bytes, offsetof(RE::ImageSpaceLUTData, texture) + index * 8,
            destination.texture);
}

bool ReadNativeName(const Slot& slot, fo4hdr::LutSnapshot& path) noexcept
{
    std::uintptr_t graphics{}, name{};
    if (slot.native > UINTPTR_MAX - sizeof(RE::NiTexture)) return false;
    return GuardedReadValue(reinterpret_cast<const void*>(slot.native +
               offsetof(RE::NiTexture, rendererTexture)), graphics) && graphics == slot.texture &&
        GuardedReadValue(reinterpret_cast<const void*>(slot.native + offsetof(RE::NiTexture, name)), name) &&
        CopyPath(reinterpret_cast<const char*>(name), path);
}

bool SameLoadedTexture(const Slot& slot, const fo4hdr::LutSnapshot& lut) noexcept
{
    if (slot.texture == 0 || slot.native == 0) return false;
    fo4hdr::LutSnapshot loaded{};
    return ReadNativeName(slot, loaded) && loaded.utf8Path == lut.utf8Path;
}

bool ValidSlotWeight(const Slot& slot) noexcept
{
    return std::isfinite(slot.weight) && slot.weight >= 0;
}

bool ReadTextureView(const Slot& slot, std::uintptr_t& view) noexcept
{
    return GuardedReadValue(reinterpret_cast<const void*>(slot.texture), view) && view != 0;
}

bool ObserveSlot(const LutBytes& bytes, std::size_t index,
    fo4hdr::NativeLutObservation& destination) noexcept
{
    Slot slot{};
    if (!ReadSlot(bytes, index, slot) || !ValidSlotWeight(slot)) return false;
    auto& lut = destination.luts[index];
    // Names remain useful as copied diagnostics even when texture identity fails.
    if (!CopyPath(reinterpret_cast<const char*>(slot.name), lut)) return false;
    lut.weight = slot.weight;
    if (!SameLoadedTexture(slot, lut)) return false;
    std::uintptr_t view{};
    if (!ReadTextureView(slot, view)) return false;
    lut.engineTextureId = slot.texture;
    destination.expectedShaderViews[index] = view;
    return true;
}

const RE::ImageSpaceLUTData* ActiveLut(const RE::ImageSpaceManager* manager) noexcept
{
    std::uintptr_t overrideLut{};
    const auto address = reinterpret_cast<std::uintptr_t>(manager);
    if (address == 0 || address > UINTPTR_MAX - sizeof(RE::ImageSpaceManager)) return nullptr;
    if (!GuardedReadValue(reinterpret_cast<const void*>(address +
            offsetof(RE::ImageSpaceManager, overrideLUTData)), overrideLut)) return nullptr;
    return reinterpret_cast<const RE::ImageSpaceLUTData*>(overrideLut != 0 ? overrideLut :
        address + offsetof(RE::ImageSpaceManager, lutData));
}

bool LutUnchanged(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, const LutBytes& bytes) noexcept
{
    if (ActiveLut(manager) != data) return false;
    LutBytes after{};
    return GuardedReadValue(data, after) && after == bytes;
}

fo4hdr::NativeLutObservation CopyLutSlots(const LutBytes& bytes) noexcept
{
    fo4hdr::NativeLutObservation candidate{};
    for (std::size_t index = 0; index < candidate.luts.size(); ++index)
        if (ObserveSlot(bytes, index, candidate)) candidate.validSlots |= 1U << index;
    return candidate;
}

}  // namespace

namespace fo4hdr::game {

bool ObserveNativeLuts(const RE::ImageSpaceManager* manager,
    std::uint64_t resourceGeneration, std::size_t textureSrvOffset,
    NativeLutObservation& destination) noexcept
{
    destination = {};
    // This decoder supports only the inspected 1.10.163 override. A later
    // runtime cannot obtain identity by selecting an arbitrary texture offset.
    if (textureSrvOffset != 0) return false;
    const auto* data = ActiveLut(manager);
    LutBytes bytes{};
    if (data == nullptr || !GuardedReadValue(data, bytes)) return false;
    auto candidate = CopyLutSlots(bytes);
    if (!LutUnchanged(manager, data, bytes)) return false;
    candidate.threadId = ::GetCurrentThreadId();
    candidate.resourceGeneration = resourceGeneration;
    destination = candidate;
    return true;
}

}  // namespace fo4hdr::game

namespace {
using Render = void (*)(RE::ImageSpaceEffect*, RE::BSTriShape*, RE::ImageSpaceEffectParam*);
std::atomic<bool> g_attempted{}, g_active{};
std::atomic<Render> g_next{};
fo4hdr::game::NativeLutRuntimeFacts g_facts{};
RE::ImageSpaceManager** g_manager{};
std::uintptr_t g_vtable{};

bool InstallationRegion(const void* address, std::size_t size,
    MEMORY_BASIC_INFORMATION& region) noexcept
{
    if (!IsReadableMemory(address, size) || !::VirtualQuery(address, &region, sizeof(region))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return start >= base && start - base <= region.RegionSize && size <= region.RegionSize - (start - base);
}

bool EffectMatches(RE::ImageSpaceEffect* effect) noexcept
{
    std::uintptr_t table{};
    return GuardedReadValue(effect, table) && table == g_vtable;
}

bool ReadObservedEffect(RE::ImageSpaceEffect* effect,
    fo4hdr::NativeLutObservation& candidate) noexcept
{
    if (!g_active.load(std::memory_order_acquire) || !EffectMatches(effect)) return false;
    RE::ImageSpaceManager* manager{};
    if (!GuardedReadValue(g_manager, manager)) return false;
    return fo4hdr::game::ObserveNativeLuts(manager, fo4hdr::NativeLutResourceGeneration(),
        g_facts.textureSrvOffset, candidate);
}

void RenderHook(RE::ImageSpaceEffect* effect, RE::BSTriShape* geometry,
    RE::ImageSpaceEffectParam* parameters) noexcept
{
    const auto next = g_next.load(std::memory_order_acquire);
    if (next == nullptr || next == &RenderHook) return;
    try
    {
        fo4hdr::NativeLutObservation candidate{};
        (void)ReadObservedEffect(effect, candidate);
        fo4hdr::NativeLutScope scope(candidate);
        next(effect, geometry, parameters);
    }
    catch (...) { g_active.store(false, std::memory_order_release); }
}
std::uintptr_t Resolve(const REL::Offset2ID& mappings, std::uintptr_t base, std::uint64_t id)
{
    for (const auto& entry : mappings)
        if (entry.id == id) return entry.offset <= UINTPTR_MAX - base ? base + entry.offset : 0;
    return 0;
}
bool InSection(const REX::FModule& module, const char* name,
    std::uintptr_t address, std::size_t size)
{
    const auto section = module.GetSection(name);
    if (address < section.GetAddress() || size > section.GetSize()) return false;
    if (address - section.GetAddress() > section.GetSize() - size) return false;
    MEMORY_BASIC_INFORMATION info{};
    return InstallationRegion(reinterpret_cast<void*>(address), size, info) &&
        info.AllocationBase == reinterpret_cast<void*>(module.GetBaseAddress()) && info.Type == MEM_IMAGE;
}

bool TargetAddressMatches(const REX::FModule& module, std::uintptr_t target) noexcept
{
    if (g_facts.targetRva > UINTPTR_MAX - module.GetBaseAddress()) return false;
    return target == module.GetBaseAddress() + g_facts.targetRva;
}

bool ReadRenderCode(const REX::FModule& module, std::uintptr_t target,
    std::array<std::uint8_t, 25>& bytes)
{
    if (!InSection(module, ".text", target, bytes.size())) return false;
    MEMORY_BASIC_INFORMATION codeInfo{};
    if (!InstallationRegion(reinterpret_cast<void*>(target), bytes.size(), codeInfo)) return false;
    if ((codeInfo.Protect & 0xFF) != PAGE_EXECUTE_READ) return false;
    return ReadBytes(reinterpret_cast<void*>(target), bytes.data(), bytes.size());
}

bool ValidRenderTarget(const REX::FModule& module, std::uintptr_t target)
{
    std::array<std::uint8_t, 25> bytes{};
    if (!TargetAddressMatches(module, target)) return false;
    return ReadRenderCode(module, target, bytes) && bytes == g_facts.targetBytes;
}

bool ValidLocator(const REX::FModule& module, std::uintptr_t table)
{
    std::uintptr_t locator{};
    std::uint32_t subobjectOffset{};
    if (!ReadBytes(reinterpret_cast<void*>(table - sizeof(void*)), &locator, sizeof(locator))) return false;
    if (!InSection(module, ".rdata", locator, 12)) return false;
    return ReadBytes(reinterpret_cast<void*>(locator + 4), &subobjectOffset, sizeof(subobjectOffset)) &&
        subobjectOffset == g_facts.effectSubobjectOffset;
}

bool ReadRenderSlot(void** slot, void*& original)
{
    MEMORY_BASIC_INFORMATION slotInfo{};
    if (!InstallationRegion(slot, sizeof(void*), slotInfo)) return false;
    if (reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) != 0) return false;
    return ReadBytes(slot, &original, sizeof(original));
}
void RestoreRenderSlot(void** slot, void* original, DWORD prior)
{
    DWORD ignored{};
    if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &ignored))
        ::InterlockedCompareExchangePointer(slot, original, reinterpret_cast<void*>(&RenderHook));
    (void)::VirtualProtect(slot, sizeof(void*), prior, &ignored);
}

bool InstallRenderSlot(void** slot, void* original)
{
    DWORD prior{};
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior)) return false;
    // The caller positively admits exclusive/pre-use ownership. The pinned
    // callback remains callable on rollback failure; never replace a foreign hook.
    const auto replaced = ::InterlockedCompareExchangePointer(slot,
        reinterpret_cast<void*>(&RenderHook), original) == original;
    DWORD ignored{};
    const auto restored = ::VirtualProtect(slot, sizeof(void*), prior, &ignored) != FALSE;
    if (!replaced || !restored) {
        if (replaced) RestoreRenderSlot(slot, original, prior);
        else (void)::VirtualProtect(slot, sizeof(void*), prior, &ignored);
        return false;
    }

    return true;
}
bool InstallQualifiedTarget(std::uintptr_t table, std::uintptr_t manager, std::uintptr_t target)
{
    
    auto* slot = reinterpret_cast<void**>(table + sizeof(void*));
    void* original{};
    if (!ReadRenderSlot(slot, original)) return false;
    if (original != reinterpret_cast<void*>(target)) {
        return false;
    }
    g_manager = reinterpret_cast<RE::ImageSpaceManager**>(manager);
    g_vtable = table;
    g_next.store(reinterpret_cast<Render>(target), std::memory_order_release);
    if (!InstallRenderSlot(slot, original)) return false;
    g_active.store(true, std::memory_order_release);
    return true;
}
bool ResolveAndInstall(const REX::FModule& module)
{
    
    const auto base = module.GetBaseAddress();
    REL::Offset2ID mappings;
    mappings.load_v2();
    const auto table = Resolve(mappings, base, RE::VTABLE::BSImagespaceShaderGammaCorrectLUT[2].id());
    const auto manager = Resolve(mappings, base, 161743);
    const auto target = Resolve(mappings, base, g_facts.targetId);
    if (!InSection(module, ".rdata", table - sizeof(void*), 3 * sizeof(void*))) return false;
    if (!InSection(module, ".data", manager, sizeof(void*))) return false;
    if (!ValidRenderTarget(module, target)) return false;
    if (!ValidLocator(module, table)) return false;
    return InstallQualifiedTarget(table, manager, target);
}

bool PinCallback(const REX::FModule& module)
{
    if (!module.GetBaseAddress() || module.GetFileVersion() != REL::Version{1, 10, 163, 0}) return false;
    HMODULE resident{};
    return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&RenderHook), &resident) != FALSE;
}
}  // namespace

namespace fo4hdr::game {
bool InitializeNativeLutObserver(const NativeLutInstallation& installation) noexcept
{
    if (!installation.Admitted()) return false;
    if (installation.facts.textureSrvOffset != 0 || installation.facts.effectSubobjectOffset != 0x118) return false;
    if (g_attempted.exchange(true, std::memory_order_acq_rel)) return g_active.load(std::memory_order_acquire);
    try
    {
        const auto module = REX::FModule::GetExecutingModule();
        if (!PinCallback(module)) return false;
        g_facts = installation.facts;
        return ResolveAndInstall(module);
    }
    catch (...) { return false; }
}
}  // namespace fo4hdr::game
