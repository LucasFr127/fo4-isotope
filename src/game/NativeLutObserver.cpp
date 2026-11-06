// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "NativeLutObserver.hpp"
#include "DirectCallPatch.hpp"
#include "ImageSpaceObserver.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/output/NativeLutObservation.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <span>
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.10.984/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.11.240/RuntimeIdentity.generated.hpp"

namespace {
using Render = void (*)(RE::ImageSpaceEffect*, RE::BSTriShape*, RE::ImageSpaceEffectParam*);
using fo4hdr::NativeImageSpacePass;
std::atomic<bool> g_attempted{};
std::atomic<Render> g_next{};
// FXAA overrides the shared image-space Render with its own.
std::atomic<Render> g_nextFxaa{};
// One bit per NativeImageSpacePass whose scope may open.
std::atomic<std::uint32_t> g_readyScopes{};
RE::ImageSpaceManager** g_manager{};
std::uintptr_t g_vtable{};

constexpr std::uint32_t ScopeBit(NativeImageSpacePass pass) noexcept
{
    return std::uint32_t{1} << static_cast<std::uint8_t>(pass);
}
// A group's scopes open together: a lone tone map would feed a foreign LUT.
constexpr auto kToneScopes = ScopeBit(NativeImageSpacePass::lut) |
    ScopeBit(NativeImageSpacePass::toneMapCinematic) | ScopeBit(NativeImageSpacePass::toneMapCinematicFade);
constexpr auto kTemporalScopes = ScopeBit(NativeImageSpacePass::temporalAa) |
    ScopeBit(NativeImageSpacePass::temporalAaPipboy) | ScopeBit(NativeImageSpacePass::temporalAaPowerArmorPipboy);
constexpr auto kDepthOfFieldScopes =
    ScopeBit(NativeImageSpacePass::bokehDofBlend) | ScopeBit(NativeImageSpacePass::distantBlur);
constexpr auto kFxaaScopes = ScopeBit(NativeImageSpacePass::fxaa);

constexpr std::uint32_t ScopeGroup(NativeImageSpacePass pass) noexcept
{
    for (const auto group : {kToneScopes, kTemporalScopes, kDepthOfFieldScopes, kFxaaScopes})
        if ((group & ScopeBit(pass)) != 0) return group;
    return 0;
}

// Opens the D3D-side pass scope only while its group's slots are installed.
class PassScope
{
public:
    explicit PassScope(NativeImageSpacePass pass) noexcept :
        active_((g_readyScopes.load(std::memory_order_acquire) & ScopeBit(pass)) != 0),
        previous_(active_ ? fo4hdr::EnterNativeImageSpacePass(pass) : NativeImageSpacePass::none) {}
    ~PassScope() { if (active_) fo4hdr::ExitNativeImageSpacePass(previous_); }
    PassScope(const PassScope&) = delete;
    PassScope& operator=(const PassScope&) = delete;

private:
    bool active_;
    NativeImageSpacePass previous_;
};
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
    return reinterpret_cast<std::uintptr_t>(texture->srView);
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
    result.generation = fo4hdr::NativeLutObservationGeneration();
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
        const PassScope pass(NativeImageSpacePass::lut);
        next(effect, geometry, parameters);
    } catch (...) {
        fo4hdr::SetNativeLutObserverStatus(fo4hdr::NativeLutObserverStatus::unavailable);
    }
}
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

struct LutIdentity
{
    std::uint64_t renderId{};
    std::size_t subobjectOffset{};
    std::uint64_t fxaaRenderId{};
};
constexpr LutIdentity kOgLut{fo4hdr::generated::og::kNativeLutRenderId,
    fo4hdr::generated::og::kNativeLutEffectSubobjectOffset, fo4hdr::generated::og::kNativeFxaaRenderId};
constexpr LutIdentity kAeLut{fo4hdr::generated::ae::kNativeLutRenderId,
    fo4hdr::generated::ae::kNativeLutEffectSubobjectOffset, fo4hdr::generated::ae::kNativeFxaaRenderId};
constexpr LutIdentity kNgLut{fo4hdr::generated::ng::kNativeLutRenderId,
    fo4hdr::generated::ng::kNativeLutEffectSubobjectOffset, fo4hdr::generated::ng::kNativeFxaaRenderId};

const LutIdentity& RuntimeLutIdentity(const fo4hdr::RuntimeProfile& profile) noexcept
{
    if (profile.key == "1.10.163") return kOgLut;
    if (profile.key == "1.10.984") return kNgLut;
    return kAeLut;
}

// The installed slot must still hold this ID's function; see InstallQualifiedTarget.
bool ValidRenderTarget(const REX::FModule& module, std::uintptr_t target)
{
    if (!target || !InSection(module, ".text", target, 1)) return false;
    MEMORY_BASIC_INFORMATION codeInfo{};
    if (!Region(reinterpret_cast<void*>(target), 1, codeInfo)) return false;
    return (codeInfo.Protect & 0xFF) == PAGE_EXECUTE_READ;
}

bool ValidLocator(const REX::FModule& module, std::uintptr_t table, const LutIdentity& identity)
{
    std::uintptr_t locator{};
    std::uint32_t subobjectOffset{};
    if (!Read(reinterpret_cast<void*>(table - sizeof(void*)), &locator, sizeof(locator))) return false;
    if (!InSection(module, ".rdata", locator, 12)) return false;
    return Read(reinterpret_cast<void*>(locator + 4), &subobjectOffset, sizeof(subobjectOffset)) &&
        subobjectOffset == identity.subobjectOffset;
}

bool ReadRenderSlot(void** slot, void*& original)
{
    MEMORY_BASIC_INFORMATION slotInfo{};
    if (!Region(slot, sizeof(void*), slotInfo)) return false;
    if (reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) != 0) return false;
    return Read(slot, &original, sizeof(original));
}
void RestoreRenderSlot(void** slot, void* original, void* hook, bool replaced)
{
    DWORD writablePrior{};
    if (!replaced || !::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &writablePrior)) return;
    ::InterlockedCompareExchangePointer(slot, original, hook);
    DWORD ignored{};
    ::VirtualProtect(slot, sizeof(void*), writablePrior, &ignored);
}

enum class SlotSwap { installed, conflicting, failed };

SlotSwap SwapRenderSlot(void** slot, void* original, void* hook)
{
    DWORD prior{};
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior)) return SlotSwap::failed;
    // F4SE startup precedes render dispatch. The process-lifetime callback
    // remains callable on rollback failure; never overwrite a foreign owner.
    const auto replaced = ::InterlockedCompareExchangePointer(slot, hook, original) == original;
    DWORD ignored{};
    const auto restored = ::VirtualProtect(slot, sizeof(void*), prior, &ignored) != FALSE;
    if (replaced && restored) return SlotSwap::installed;
    RestoreRenderSlot(slot, original, hook, replaced);
    if (!restored) ::VirtualProtect(slot, sizeof(void*), prior, &ignored);
    return replaced ? SlotSwap::failed : SlotSwap::conflicting;
}

bool InstallRenderSlot(void** slot, void* original)
{
    using fo4hdr::SetNativeLutObserverStatus;
    using Status = fo4hdr::NativeLutObserverStatus;
    const auto swapped = SwapRenderSlot(slot, original, reinterpret_cast<void*>(&RenderHook));
    if (swapped == SlotSwap::installed) return true;
    SetNativeLutObserverStatus(swapped == SlotSwap::conflicting ? Status::conflictingHook : Status::invalidTarget);
    return false;
}

template <NativeImageSpacePass Pass>
std::atomic<Render>& PassTarget() noexcept
{
    if constexpr (Pass == NativeImageSpacePass::fxaa) return g_nextFxaa;
    else return g_next;
}

template <NativeImageSpacePass Pass>
void PassRenderHook(RE::ImageSpaceEffect* effect, RE::BSTriShape* geometry,
    RE::ImageSpaceEffectParam* parameters) noexcept
{
    const auto next = PassTarget<Pass>().load(std::memory_order_acquire);
    if (!next || next == &PassRenderHook<Pass>) return;
    try {
        const PassScope scope(Pass);
        next(effect, geometry, parameters);
    } catch (...) {
        g_readyScopes.fetch_and(~ScopeGroup(Pass), std::memory_order_acq_rel);
    }
}

struct PassSlot
{
    std::uint64_t vtableId;
    void* hook;
    void** slot{};
};

template <NativeImageSpacePass Pass>
PassSlot ScopeSlot(std::uint64_t vtableId) noexcept
{
    return {vtableId, reinterpret_cast<void*>(&PassRenderHook<Pass>)};
}

// The executable, its Address Library mappings and the runtime facts that qualify a slot.
struct SlotContext
{
    const REX::FModule& module;
    const REL::Offset2ID& mappings;
    const LutIdentity& identity;
};

bool QualifyPassSlot(const SlotContext& context, PassSlot& entry, void* expected)
{
    const auto table = Resolve(context.mappings, context.module.GetBaseAddress(), entry.vtableId);
    if (!InSection(context.module, ".rdata", table - sizeof(void*), 3 * sizeof(void*))) return false;
    if (!ValidLocator(context.module, table, context.identity)) return false;
    entry.slot = reinterpret_cast<void**>(table + sizeof(void*));
    void* original{};
    return ReadRenderSlot(entry.slot, original) && original == expected;
}

void RestorePassSlots(std::span<PassSlot> slots, void* expected)
{
    for (const auto& entry : slots) RestoreRenderSlot(entry.slot, expected, entry.hook, true);
}

bool SwapPassSlots(std::span<PassSlot> slots, void* expected)
{
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (SwapRenderSlot(slots[index].slot, expected, slots[index].hook) == SlotSwap::installed) continue;
        RestorePassSlots(slots.first(index), expected);
        return false;
    }
    return true;
}

// Every slot of a group must hold the same target under the effect's sub-object
// offset; the group swaps together or rolls back.
bool InstallPassGroup(const SlotContext& context, std::span<PassSlot> slots, std::uintptr_t target)
{
    auto* const expected = reinterpret_cast<void*>(target);
    for (auto& entry : slots)
        if (!QualifyPassSlot(context, entry, expected)) return false;
    return SwapPassSlots(slots, expected);
}

// FXAA's slot holds its own manifest-qualified Render instead of the shared one.
bool InstallFxaaScope(const SlotContext& context, std::uintptr_t sharedTarget)
{
    const auto base = context.module.GetBaseAddress();
    const auto rva = fo4hdr::game::direct_call_patch::ResolveMapping(
        context.mappings, base, context.identity.fxaaRenderId);
    const auto target = rva ? base + rva : 0;
    if (target == sharedTarget || !ValidRenderTarget(context.module, target)) return false;
    g_nextFxaa.store(reinterpret_cast<Render>(target), std::memory_order_release);
    std::array slots{ScopeSlot<NativeImageSpacePass::fxaa>(RE::VTABLE::BSImagespaceShaderFXAA[2].id())};
    return InstallPassGroup(context, slots, target);
}

// The tone-map, temporal and depth-of-field classes inherit the LUT's image-space
// Render, so the same target and sub-object offset qualify their slots.
std::uint32_t InstallPassScopes(const SlotContext& context, std::uintptr_t target)
{
    using enum NativeImageSpacePass;
    std::array tone{ScopeSlot<toneMapCinematic>(RE::VTABLE::BSImagespaceShaderHDRTonemapBlendCinematic[2].id()),
        ScopeSlot<toneMapCinematicFade>(RE::VTABLE::BSImagespaceShaderHDRTonemapBlendCinematicFade[2].id())};
    std::array temporal{ScopeSlot<temporalAa>(RE::VTABLE::BSImagespaceShaderTemporalAA[2].id()),
        ScopeSlot<temporalAaPipboy>(RE::VTABLE::BSImagespaceShaderTemporalAAPipboy[2].id()),
        ScopeSlot<temporalAaPowerArmorPipboy>(RE::VTABLE::BSImagespaceShaderTemporalAAPowerArmorPipboy[2].id())};
    std::array depthOfField{
        ScopeSlot<bokehDofBlend>(RE::VTABLE::BSImagespaceShaderBokehDepthOfFieldPass4[2].id()),
        ScopeSlot<distantBlur>(RE::VTABLE::BSImagespaceShaderDistantBlur[2].id())};
    std::uint32_t ready{};
    if (InstallPassGroup(context, tone, target)) ready |= kToneScopes;
    if (InstallPassGroup(context, temporal, target)) ready |= kTemporalScopes;
    if (InstallPassGroup(context, depthOfField, target)) ready |= kDepthOfFieldScopes;
    if (InstallFxaaScope(context, target)) ready |= kFxaaScopes;
    return ready;
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
bool ResolveAndInstall(const REX::FModule& module, const LutIdentity& identity)
{
    const auto base = module.GetBaseAddress();
    REL::Offset2ID mappings;
    mappings.load_v2();
    const auto table = Resolve(mappings, base, RE::VTABLE::BSImagespaceShaderGammaCorrectLUT[2].id());
    const auto manager = RE::ID::ImageSpaceManager::Singleton.address();
    const auto targetRva = fo4hdr::game::direct_call_patch::ResolveMapping(
        mappings, base, identity.renderId);
    const auto target = targetRva ? base + targetRva : 0;
    if (!InSection(module, ".rdata", table - sizeof(void*), 3 * sizeof(void*))) return false;
    if (!InSection(module, ".data", manager, sizeof(void*))) return false;
    if (!ValidRenderTarget(module, target)) return false;
    if (!ValidLocator(module, table, identity)) return false;
    if (!InstallQualifiedTarget(table, manager, target)) return false;
    g_readyScopes.store(InstallPassScopes({module, mappings, identity}, target), std::memory_order_release);
    return true;
}
}

namespace fo4hdr::game {
bool NativeImageSpacePassInstalled(NativeImageSpacePass pass) noexcept
{
    return (g_readyScopes.load(std::memory_order_acquire) & ScopeBit(pass)) != 0;
}

bool InitializeNativeLutObserver(bool requested) noexcept
{
    using Status = NativeLutObserverStatus;
    if (g_attempted.exchange(true)) return GetNativeLutObserverStatus() == Status::ready;
    if (!requested) return false;
    SetNativeLutObserverStatus(Status::unsupportedRuntime);
    try {
        const auto* profile = ActiveRuntimeProfile();
        if (!profile) return false;
        const auto module = REX::FModule::GetExecutingModule();
        if (!module.GetBaseAddress()) return false;
        SetNativeLutObserverStatus(Status::invalidTarget);
        return ResolveAndInstall(module, RuntimeLutIdentity(*profile));
    } catch (...) { return false; }
}
}
