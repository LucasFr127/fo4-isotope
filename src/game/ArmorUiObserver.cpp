// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "ArmorUiObserver.hpp"
#include "DirectCallPatch.hpp"
#include "FO4Isotope/output/ArmorUiComposition.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.10.984/RuntimeIdentity.generated.hpp"
#include "../../runtime/1.11.240/RuntimeIdentity.generated.hpp"

namespace {
namespace detail = fo4hdr::game::direct_call_patch;
using ScreenSubmit = void (*)(RE::Interface3D::Renderer*, std::uint32_t);
using ScreenUiSubmit = void (*)(RE::UI*);
template<class Submit>
struct ObserverState
{
    std::atomic<bool> attempted{}, installed{};
    std::atomic<Submit> next{};
};
struct TrampolineInterface
{
    std::uint32_t version{}, padding{};
    void* (__cdecl* branch)(std::uint32_t, std::size_t){};
    void* (__cdecl* local)(std::uint32_t, std::size_t){};
};
static_assert(sizeof(TrampolineInterface) == 0x18);
static_assert(offsetof(TrampolineInterface, branch) == 8);
static_assert(offsetof(RE::Interface3D::Renderer, name) == 0x208);
static_assert(offsetof(RE::Interface3D::Renderer, postAA) == 0x53);
static_assert(offsetof(RE::Interface3D::Renderer, screenAttachedElementRoot) == 0x80);
static_assert(sizeof(RE::BSFixedString) == sizeof(void*));
ObserverState<ScreenSubmit> g_armorObserver;
ObserverState<ScreenUiSubmit> g_screenObserver;
// Written once before installation's release; ObserveScope acquires before reading.
bool g_flatKeyReady{}, g_armorKeyReady{}, g_rainKeyReady{}, g_workshopKeyReady{};
std::array<std::uintptr_t, 5> g_keyRvas{};

bool Read(const void* source, void* destination, std::size_t size) noexcept
{
    SIZE_T copied{};
    return source && ::ReadProcessMemory(::GetCurrentProcess(), source,
        destination, size, &copied) && copied == size;
}

bool ContainsBytes(std::uintptr_t base, std::size_t extent, std::uintptr_t address, std::size_t size) noexcept
{
    return address >= base && address - base < extent && size <= extent - (address - base);
}
bool Region(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& info) noexcept
{
    if (!address || !size) return false;
    if (!::VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return ContainsBytes(base, info.RegionSize, start, size);
}

bool BelongsToModule(const MEMORY_BASIC_INFORMATION& info, const void* module) noexcept
{ return info.AllocationBase == module && info.Type == MEM_IMAGE; }
bool ExecutableProtection(DWORD flags) noexcept
{
    const auto protection = flags & 0xFF;
    return protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
}

bool ReadableDataProtection(DWORD flags) noexcept
{
    const auto protection = flags & 0xFF;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY;
}

bool Executable(const void* address, std::size_t size, const void* module = nullptr) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!Region(address, size, info)) return false;
    if (module && !BelongsToModule(info, module)) return false;
    return ExecutableProtection(info.Protect);
}

bool MatchesBytes(std::uintptr_t address, std::span<const std::uint8_t> expected) noexcept
{
    return fo4hdr::game::detail::MatchesNativeCode(address, expected, Read);
}

// Startup-only mutation. Each patch is contained in one memory region/page,
// and the F4SE load stage precedes use of the native rendering callers.
bool WriteBytes(std::uintptr_t address, std::span<const std::uint8_t> bytes,
    DWORD originalProtection) noexcept
{
    DWORD prior{};
    if (!::VirtualProtect(reinterpret_cast<void*>(address), bytes.size(),
        PAGE_EXECUTE_READWRITE, &prior)) return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
    DWORD ignored{};
    const auto restored = ::VirtualProtect(reinterpret_cast<void*>(address), bytes.size(),
        originalProtection, &ignored) != FALSE;
    const auto flushed = ::FlushInstructionCache(::GetCurrentProcess(),
        reinterpret_cast<void*>(address), bytes.size()) != FALSE;
    MEMORY_BASIC_INFORMATION info{};
    return restored && flushed && Region(reinterpret_cast<void*>(address), bytes.size(), info) &&
        info.Protect == originalProtection && MatchesBytes(address, bytes);
}

struct PatchBackend
{
    bool Matches(std::uintptr_t address, const detail::CallBytes& bytes) const noexcept
    { return MatchesBytes(address, bytes); }
    bool Replace(const detail::Site& site, const detail::CallBytes& expected,
        const detail::CallBytes& replacement) const noexcept
    {
        if (!Matches(site.address, expected)) return false;
        return WriteBytes(site.address, replacement, site.protection);
    }
};

struct RendererObservation
{
    fo4hdr::ArmorUiOwner owner{fo4hdr::ArmorUiOwner::other};
    bool postAa{}, hasRoot{};
    std::array<char, 32> rendererName{};
};

bool IsRendererName(const RE::BSFixedString& name, std::uintptr_t keyRva, bool ready) noexcept
{
    if (!ready) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    const auto& key = *reinterpret_cast<const RE::BSFixedString*>(base + keyRva);
    return fo4hdr::game::detail::SameNativeRendererKey(name, key);
}

fo4hdr::ArmorUiOwner RendererOwner(const RE::BSFixedString& name) noexcept
{
    if (IsRendererName(name, g_keyRvas[0], true)) return fo4hdr::ArmorUiOwner::hud;
    if (IsRendererName(name, g_keyRvas[1], g_flatKeyReady))
        return fo4hdr::ArmorUiOwner::flatMenu;
    if (IsRendererName(name, g_keyRvas[2], g_armorKeyReady))
        return fo4hdr::ArmorUiOwner::powerArmor;
    if (IsRendererName(name, g_keyRvas[3], g_rainKeyReady))
        return fo4hdr::ArmorUiOwner::powerArmorRain;
    if (IsRendererName(name, g_keyRvas[4], g_workshopKeyReady))
        return fo4hdr::ArmorUiOwner::workshop;
    return fo4hdr::ArmorUiOwner::other;
}

// No registry lookup/lock recursion, NiPointer copy, root dereference, or
// retained pointer. Native callers hold the registry read lock for this scope.
// Fault containment is for these optional reads only, never native submission.
bool Observe(RE::Interface3D::Renderer* renderer, RendererObservation& result) noexcept
{
    __try {
        if (!renderer) return false;
        result.owner = RendererOwner(renderer->name);
        if (fo4hdr::ArmorUiObservationEnabled()) {
            const auto* name = renderer->name.c_str();
            for (std::size_t i = 0; i + 1 < result.rendererName.size(); ++i) {
                result.rendererName[i] = name[i];
                if (name[i] == '\0') break;
            }
        }
        result.postAa = renderer->postAA;
        result.hasRoot = renderer->screenAttachedElementRoot.get() != nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ObserveScope(const std::atomic<bool>& installed) noexcept
{
    return installed.load(std::memory_order_acquire) &&
        (fo4hdr::ArmorUiScopeEnabled() || fo4hdr::CurrentArmorUiScope().serial != 0);
}
void SubmitObserved(RE::Interface3D::Renderer* renderer, std::uint32_t argument, ScreenSubmit next)
{
    RendererObservation observation{};
    if (!Observe(renderer, observation)) observation = {};
    fo4hdr::ArmorUiScope scope({.owner = observation.owner,
        .threadId = ::GetCurrentThreadId(), .targetArgument = argument,
        .postAa = observation.postAa, .hasRoot = observation.hasRoot,
        .rendererName = observation.rendererName});
    next(renderer, argument);
}

void ScreenSubmitHook(RE::Interface3D::Renderer* renderer, std::uint32_t argument) noexcept
{
    const auto next = g_armorObserver.next.load(std::memory_order_acquire);
    // Fail closed if forwarding publication is unavailable or corrupt; never recurse.
    if (!next || next == &ScreenSubmitHook) return;
    try {
        if (ObserveScope(g_armorObserver.installed)) {
            SubmitObserved(renderer, argument, next);
        } else {
            next(renderer, argument);
        }
    } catch (...) {
        g_armorObserver.installed.store(false, std::memory_order_release);
    }
}

void ScreenUiSubmitHook(RE::UI* ui) noexcept
{
    const auto next = g_screenObserver.next.load(std::memory_order_acquire);
    if (!next || next == &ScreenUiSubmitHook) return;
    try {
        if (ObserveScope(g_screenObserver.installed)) {
            // Entry marks the native screen-space phase; queued work can outlive this call.
            fo4hdr::ArmorUiScope scope({.owner = fo4hdr::ArmorUiOwner::screenSpace,
                .threadId = ::GetCurrentThreadId()});
            next(ui);
        } else {
            next(ui);
        }
    } catch (...) {
        g_screenObserver.installed.store(false, std::memory_order_release);
    }
}
struct RendererKeyIdentity
{
    std::uint64_t storageId{};
    std::uintptr_t storageRva{};
    std::uint64_t codeId{};
    std::uintptr_t codeRva{};
    std::span<const std::uint8_t> codeBytes;
};
template<std::size_t N>
struct BoundaryIdentity
{
    std::uintptr_t targetRva{};
    std::uint64_t targetId{};
    std::span<const std::uint8_t> targetBytes;
    std::array<std::uintptr_t, N> callerRvas{};
    std::array<std::uint64_t, N> callerIds{};
    std::array<std::uintptr_t, N> callRvas{};
    std::array<detail::CallBytes, N> callBytes{};
    const std::array<RendererKeyIdentity, 5>* rendererKeys{};
};
namespace og {
using namespace fo4hdr::generated::og;
constexpr std::array<RendererKeyIdentity, 5> kKeys{{
    {kHudUiKeyStorageId, kHudUiKeyStorageRva, kHudUiKeyGetterId, kHudUiKeyGetterRva, kHudUiKeyGetterBytes},
    {kFlatUiKeyStorageId, kFlatUiKeyStorageRva, kFlatUiKeyGetterId, kFlatUiKeyGetterRva, kFlatUiKeyGetterBytes},
    {kArmorUiKeyStorageId, kArmorUiKeyStorageRva, kArmorUiKeyGetterId, kArmorUiKeyGetterRva, kArmorUiKeyGetterBytes},
    {kRainUiKeyStorageId, kRainUiKeyStorageRva, kRainUiKeyGetterId, kRainUiKeyGetterRva, kRainUiKeyGetterBytes},
    {kWorkshopUiKeyStorageId, kWorkshopUiKeyStorageRva, kWorkshopUiKeyInitializerId,
        kWorkshopUiKeyInitializerRva, kWorkshopUiKeyInitializerBytes}
}};
constexpr BoundaryIdentity<kArmorUiCallRvas.size()> kArmor{kArmorUiTargetRva, kArmorUiTargetId,
    kArmorUiTargetBytes, kArmorUiCallerRvas, kArmorUiCallerIds, kArmorUiCallRvas, kArmorUiCallBytes, &kKeys};
constexpr BoundaryIdentity<kScreenUiCallRvas.size()> kScreen{kScreenUiTargetRva, kScreenUiTargetId,
    kScreenUiTargetBytes, kScreenUiCallerRvas, kScreenUiCallerIds, kScreenUiCallRvas, kScreenUiCallBytes};
}
namespace ng {
using namespace fo4hdr::generated::ng;
constexpr std::array<RendererKeyIdentity, 5> kKeys{{
    {kHudUiKeyStorageId, kHudUiKeyStorageRva, kHudUiKeyGetterId, kHudUiKeyGetterRva, kHudUiKeyGetterBytes},
    {kFlatUiKeyStorageId, kFlatUiKeyStorageRva, kFlatUiKeyGetterId, kFlatUiKeyGetterRva, kFlatUiKeyGetterBytes},
    {kArmorUiKeyStorageId, kArmorUiKeyStorageRva, kArmorUiKeyGetterId, kArmorUiKeyGetterRva, kArmorUiKeyGetterBytes},
    {kRainUiKeyStorageId, kRainUiKeyStorageRva, kRainUiKeyGetterId, kRainUiKeyGetterRva, kRainUiKeyGetterBytes},
    {kWorkshopUiKeyStorageId, kWorkshopUiKeyStorageRva, kWorkshopUiKeyInitializerId,
        kWorkshopUiKeyInitializerRva, kWorkshopUiKeyInitializerBytes}
}};
constexpr BoundaryIdentity<kArmorUiCallRvas.size()> kArmor{kArmorUiTargetRva, kArmorUiTargetId,
    kArmorUiTargetBytes, kArmorUiCallerRvas, kArmorUiCallerIds, kArmorUiCallRvas, kArmorUiCallBytes, &kKeys};
constexpr BoundaryIdentity<kScreenUiCallRvas.size()> kScreen{kScreenUiTargetRva, kScreenUiTargetId,
    kScreenUiTargetBytes, kScreenUiCallerRvas, kScreenUiCallerIds, kScreenUiCallRvas, kScreenUiCallBytes};
}
namespace ae {
using namespace fo4hdr::generated::ae;
constexpr std::array<RendererKeyIdentity, 5> kKeys{{
    {kHudUiKeyStorageId, kHudUiKeyStorageRva, kHudUiKeyGetterId, kHudUiKeyGetterRva, kHudUiKeyGetterBytes},
    {kFlatUiKeyStorageId, kFlatUiKeyStorageRva, kFlatUiKeyGetterId, kFlatUiKeyGetterRva, kFlatUiKeyGetterBytes},
    {kArmorUiKeyStorageId, kArmorUiKeyStorageRva, kArmorUiKeyGetterId, kArmorUiKeyGetterRva, kArmorUiKeyGetterBytes},
    {kRainUiKeyStorageId, kRainUiKeyStorageRva, kRainUiKeyGetterId, kRainUiKeyGetterRva, kRainUiKeyGetterBytes},
    {kWorkshopUiKeyStorageId, kWorkshopUiKeyStorageRva, kWorkshopUiKeyInitializerId,
        kWorkshopUiKeyInitializerRva, kWorkshopUiKeyInitializerBytes}
}};
constexpr BoundaryIdentity<kArmorUiCallRvas.size()> kArmor{kArmorUiTargetRva, kArmorUiTargetId,
    kArmorUiTargetBytes, kArmorUiCallerRvas, kArmorUiCallerIds, kArmorUiCallRvas, kArmorUiCallBytes, &kKeys};
constexpr BoundaryIdentity<kScreenUiCallRvas.size()> kScreen{kScreenUiTargetRva, kScreenUiTargetId,
    kScreenUiTargetBytes, kScreenUiCallerRvas, kScreenUiCallerIds, kScreenUiCallRvas, kScreenUiCallBytes};
}
template<std::size_t N, class Submit>
class ObserverInstallation
{
    static_assert(N > 0);
    const BoundaryIdentity<N>& identity_;
    ObserverState<Submit>& state_;
    REX::FModule module_{REX::FModule::GetExecutingModule()};
    std::uintptr_t base_{module_.GetBaseAddress()};
    REX::FModuleSection text_{};
    REL::Offset2ID mappings_;
    TrampolineInterface allocator_{};
    std::array<detail::Site, N> sites_{};
    std::uintptr_t target_{};
    std::uintptr_t hook_{};

    bool Code(std::uintptr_t address, std::size_t size) const
    {
        return ContainsBytes(text_.GetAddress(), text_.GetSize(), address, size) &&
            Executable(reinterpret_cast<void*>(address), size, reinterpret_cast<void*>(base_));
    }
    bool Mapping(std::uint64_t id, std::uintptr_t rva) const
    {
        return std::any_of(mappings_.begin(), mappings_.end(), [&](const auto& entry) {
            return entry.id == id && entry.offset == rva;
        });
    }
    bool Target() const
    {
        return Mapping(identity_.targetId, identity_.targetRva) && target_ != hook_ &&
            Code(target_, identity_.targetBytes.size()) && MatchesBytes(target_, identity_.targetBytes);
    }
    bool MatchesRuntime() const
    {
        const auto* profile = fo4hdr::ActiveRuntimeProfile();
        return base_ && profile && module_.GetFileVersion().pack() == profile->runtimeVersion;
    }
    bool KeyStorage(std::uintptr_t rva) const
    {
        const auto data = module_.GetSection(".data");
        const auto address = base_ + rva;
        MEMORY_BASIC_INFORMATION info{};
        const void* entry{};
        if (!ContainsBytes(data.GetAddress(), data.GetSize(), address, sizeof(RE::BSFixedString)))
            return false;
        if (!Region(reinterpret_cast<void*>(address), sizeof(RE::BSFixedString), info)) return false;
        // A lazy key may still be null at installation; submission requires a non-null entry.
        return BelongsToModule(info, reinterpret_cast<void*>(base_)) &&
            ReadableDataProtection(info.Protect) &&
            Read(reinterpret_cast<void*>(address), &entry, sizeof(entry));
    }
    bool RendererKey(const RendererKeyIdentity& key) const
    {
        return Mapping(key.storageId, key.storageRva) &&
            Mapping(key.codeId, key.codeRva) && KeyStorage(key.storageRva) &&
            Code(base_ + key.codeRva, key.codeBytes.size()) &&
            MatchesBytes(base_ + key.codeRva, key.codeBytes);
    }
    bool OptionalKey(const RendererKeyIdentity& key) const noexcept
    {
        try {
            return RendererKey(key);
        } catch (...) { return false; }
    }
    bool Allocator(const void* allocatorInterface, std::uint32_t handle)
    {
        return Read(allocatorInterface, &allocator_, sizeof(allocator_)) && allocator_.version == 1 &&
            handle != UINT32_MAX && Executable(reinterpret_cast<void*>(allocator_.branch), 1);
    }
    bool PrepareSite(std::size_t i)
    {
        const auto address = base_ + identity_.callRvas[i];
        MEMORY_BASIC_INFORMATION info{};
        const bool valid = Mapping(identity_.callerIds[i], identity_.callerRvas[i]) &&
            Code(base_ + identity_.callerRvas[i], 1) && Code(address, 5) &&
            MatchesBytes(address, identity_.callBytes[i]) &&
            detail::CallTarget(address, identity_.callBytes[i]) == target_ &&
            Region(reinterpret_cast<void*>(address), 5, info);
        if (!valid) return false;
        sites_[i] = {address, identity_.callBytes[i], {}, info.Protect};
        return true;
    }
    bool PrepareRelay(detail::Site& site, std::uint32_t handle)
    {
        auto* relay = allocator_.branch(handle, 14);
        MEMORY_BASIC_INFORMATION info{};
        if (!Executable(relay, 14) || !Region(relay, 14, info)) return false;
        if (!detail::EncodeCall(site.address, reinterpret_cast<std::uintptr_t>(relay), site.replacement))
            return false;
        std::array<std::uint8_t, 14> bytes{0xFF, 0x25, 0, 0, 0, 0};
        std::memcpy(bytes.data() + 6, &hook_, sizeof(hook_));
        return WriteBytes(reinterpret_cast<std::uintptr_t>(relay), bytes, info.Protect);
    }
    bool PrepareSites(std::uint32_t handle)
    {
        for (std::size_t i = 0; i < sites_.size(); ++i)
            if (!PrepareSite(i)) return false;
        // Relays belong to F4SE's process-lifetime pool, including every failure
        // path. Publish valid forwarding before a CALL can reach a relay.
        return PrepareRelays(handle);
    }
    bool PrepareRelays(std::uint32_t handle)
    {
        for (auto& site : sites_)
            if (!PrepareRelay(site, handle)) return false;
        return true;
    }
    bool Publish()
    {
        if (!MatchesBytes(target_, identity_.targetBytes)) return false;
        state_.next.store(reinterpret_cast<Submit>(target_), std::memory_order_release);
        PatchBackend backend;
        if (!detail::InstallPair(sites_, backend)) return false;
        state_.installed.store(true, std::memory_order_release);
        return true;
    }
    bool RendererKeys()
    {
        const auto& keys = *identity_.rendererKeys;
        if (!RendererKey(keys[0])) return false;
        for (std::size_t index = 0; index < keys.size(); ++index)
            g_keyRvas[index] = keys[index].storageRva;
        g_flatKeyReady = OptionalKey(keys[1]);
        g_armorKeyReady = OptionalKey(keys[2]);
        g_rainKeyReady = OptionalKey(keys[3]);
        g_workshopKeyReady = OptionalKey(keys[4]);
        return true;
    }
public:
    ObserverInstallation(const BoundaryIdentity<N>& identity, ObserverState<Submit>& state, Submit hook) :
        identity_(identity), state_(state), target_(base_ + identity.targetRva),
        hook_(reinterpret_cast<std::uintptr_t>(hook)) {}

    bool Install(const void* allocatorInterface, std::uint32_t handle)
    {
        if (!MatchesRuntime()) return false;
        text_ = module_.GetSection(".text");
        // The pinned ID lookup terminates on a missing ID. Its public inverse
        // container permits recoverable verification without a second parser.
        mappings_.load_v2();
        if (!Target() || !Allocator(allocatorInterface, handle)) return false;
        if (identity_.rendererKeys && !RendererKeys()) return false;
        return PrepareSites(handle) && Publish();
    }
};
}

namespace fo4hdr::game {
bool InitializeArmorUiObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept
{
    if (g_armorObserver.attempted.exchange(true, std::memory_order_acq_rel))
        return g_armorObserver.installed.load(std::memory_order_acquire);
    if (!requested) return false;
    try {
        const auto* profile = ActiveRuntimeProfile();
        if (!profile) return false;
        if (IsOriginalRuntime()) {
            ObserverInstallation installation(og::kArmor, g_armorObserver,
                static_cast<ScreenSubmit>(&ScreenSubmitHook));
            return installation.Install(trampolineInterface, pluginHandle);
        }
        const auto& boundary = profile->key == "1.10.984" ? ng::kArmor : ae::kArmor;
        ObserverInstallation installation(boundary, g_armorObserver,
            static_cast<ScreenSubmit>(&ScreenSubmitHook));
        return installation.Install(trampolineInterface, pluginHandle);
    } catch (...) { return false; }
}
bool InitializeScreenUiObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept
{
    if (g_screenObserver.attempted.exchange(true, std::memory_order_acq_rel))
        return g_screenObserver.installed.load(std::memory_order_acquire);
    if (!requested) return false;
    try {
        const auto* profile = ActiveRuntimeProfile();
        if (!profile) return false;
        const auto& boundary = IsOriginalRuntime() ? og::kScreen :
            (profile->key == "1.10.984" ? ng::kScreen : ae::kScreen);
        ObserverInstallation installation(boundary, g_screenObserver,
            static_cast<ScreenUiSubmit>(&ScreenUiSubmitHook));
        return installation.Install(trampolineInterface, pluginHandle);
    } catch (...) { return false; }
}
}
