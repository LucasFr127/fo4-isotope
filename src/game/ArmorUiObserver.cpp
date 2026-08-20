// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "ArmorUiObserver.hpp"
#include "DirectCallPatch.hpp"
#include "Fallout4HDR/output/ArmorUiComposition.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
namespace {
namespace detail = fo4hdr::game::direct_call_patch;
using ScreenSubmit = void (*)(RE::Interface3D::Renderer*, std::uint32_t);
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
std::atomic<bool> g_attempted{}, g_installed{};
std::atomic<ScreenSubmit> g_next{};

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

bool Executable(const void* address, std::size_t size, const void* module = nullptr) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!Region(address, size, info)) return false;
    if (module && !BelongsToModule(info, module)) return false;
    return ExecutableProtection(info.Protect);
}

bool MatchesBytes(std::uintptr_t address, std::span<const std::uint8_t> expected) noexcept
{
    std::array<std::uint8_t, 32> actual{};
    return expected.size() <= actual.size() && Read(reinterpret_cast<void*>(address),
        actual.data(), expected.size()) && std::equal(expected.begin(), expected.end(), actual.begin());
}

// Caller admission proves a pre-use exclusive mutation window. A five-byte
// write is not atomic; the F4SE load event alone provides no such proof.
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
};

bool IsArmorName(const char* name) noexcept
{
    constexpr char armor[] = "PowerArmorRenderer";
    bool armorMatch = true;
    for (std::size_t i = 0; i < sizeof(armor); ++i) {
        const auto value = name[i];
        armorMatch = armorMatch && value == armor[i];
        if (value == '\0') break;
    }
    return armorMatch;
}

// No registry lookup/lock recursion, NiPointer copy, root dereference, or
// retained pointer. Installation admission must prove the caller-held registry
// lifetime; matching code bytes alone does not establish it.
// Fault containment is for these optional reads only, never native submission.
bool Observe(RE::Interface3D::Renderer* renderer, RendererObservation& result) noexcept
{
    __try {
        if (!renderer) return false;
        const auto* name = renderer->name.c_str();
        const auto armorMatch = IsArmorName(name);
        result.owner = armorMatch ? fo4hdr::ArmorUiOwner::powerArmor :
            fo4hdr::ArmorUiOwner::other;
        result.postAa = renderer->postAA;
        result.hasRoot = renderer->screenAttachedElementRoot.get() != nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ObserveScope() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}
void SubmitObserved(RE::Interface3D::Renderer* renderer, std::uint32_t argument, ScreenSubmit next)
{
    RendererObservation observation{};
    if (!Observe(renderer, observation)) observation = {};
    fo4hdr::ArmorUiScope scope({.owner = observation.owner,
        .threadId = ::GetCurrentThreadId(), .targetArgument = argument,
        .postAa = observation.postAa, .hasRoot = observation.hasRoot});
    next(renderer, argument);
}

void ScreenSubmitHook(RE::Interface3D::Renderer* renderer, std::uint32_t argument) noexcept
{
    const auto next = g_next.load(std::memory_order_acquire);
    // Fail closed if forwarding publication is unavailable or corrupt; never recurse.
    if (!next || next == &ScreenSubmitHook) return;
    try {
        if (ObserveScope()) {
            SubmitObserved(renderer, argument, next);
        } else {
            next(renderer, argument);
        }
    } catch (...) {
        g_installed.store(false, std::memory_order_release);
    }
}
class ArmorInstallation
{
    REX::FModule module_{REX::FModule::GetExecutingModule()};
    std::uintptr_t base_{module_.GetBaseAddress()};
    REX::FModuleSection text_{};
    REL::Offset2ID mappings_;
    TrampolineInterface allocator_{};
    std::array<detail::Site, 2> sites_{};
    const fo4hdr::game::ArmorUiRuntimeFacts& facts_;
    std::uintptr_t target_{};
    std::uintptr_t hook_{reinterpret_cast<std::uintptr_t>(&ScreenSubmitHook)};

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
        return Mapping(facts_.targetId, facts_.targetRva) && target_ != hook_ &&
            Code(target_, facts_.targetBytes.size()) && MatchesBytes(target_, facts_.targetBytes);
    }
    bool Allocator(const void* allocatorInterface, std::uint32_t handle)
    {
        return Read(allocatorInterface, &allocator_, sizeof(allocator_)) && allocator_.version == 1 &&
            handle != UINT32_MAX && Executable(reinterpret_cast<void*>(allocator_.branch), 1);
    }
    bool CallMatches(std::size_t i, std::uintptr_t address) const
    {
        const auto& call = facts_.calls[i];
        return Mapping(call.callerId, call.callerRva) && Code(base_ + call.callerRva, 1) &&
            Code(address, 5) && MatchesBytes(address, call.bytes) &&
            detail::CallTarget(address, call.bytes) == target_;
    }
    bool PrepareSite(std::size_t i)
    {
        if (facts_.calls[i].callRva > UINTPTR_MAX - base_ ||
            facts_.calls[i].callerRva > UINTPTR_MAX - base_) return false;
        const auto address = base_ + facts_.calls[i].callRva;
        MEMORY_BASIC_INFORMATION info{};
        const bool valid = CallMatches(i, address) &&
            Region(reinterpret_cast<void*>(address), 5, info);
        if (!valid) return false;
        sites_[i] = {address, facts_.calls[i].bytes, {}, info.Protect};
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
        // path. Publish valid forwarding before a CALL can reach either relay.
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
        if (!MatchesBytes(target_, facts_.targetBytes)) return false;
        g_next.store(reinterpret_cast<ScreenSubmit>(target_), std::memory_order_release);
        PatchBackend backend;
        if (!detail::InstallPair(sites_, backend)) return false;
        g_installed.store(true, std::memory_order_release);
        return true;
    }
public:
    explicit ArmorInstallation(const fo4hdr::game::ArmorUiRuntimeFacts& facts)
        : facts_(facts) {}
    bool PrepareModule()
    {
        if (!base_ || module_.GetFileVersion() != REL::Version{1, 10, 163, 0}) return false;
        if (facts_.targetRva > UINTPTR_MAX - base_) return false;
        target_ = base_ + facts_.targetRva;
        text_ = module_.GetSection(".text");
        return true;
    }
    bool Install(const void* allocatorInterface, std::uint32_t handle)
    {
        HMODULE resident{};
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&ScreenSubmitHook), &resident)) return false;
        if (!PrepareModule()) return false;
        // The pinned ID lookup terminates on a missing ID. Its public inverse
        // container permits recoverable verification without a second parser.
        mappings_.load_v2();
        if (facts_.calls[0].callRva == facts_.calls[1].callRva) return false;
        if (!Target() || !Allocator(allocatorInterface, handle)) return false;
        if (!PrepareSites(handle)) return false;
        return Publish();
    }
};
}

namespace fo4hdr::game {
namespace {
bool Admitted(const ArmorUiInstallation& installation) noexcept
{
    return installation.runtimeAuthorized && installation.preUseExclusive &&
        installation.abiAndRegistryLifetimeProven && installation.addressLibraryAvailable &&
        installation.trampolineOwned;
}
}
bool InitializeArmorUiObserver(const ArmorUiInstallation& installation) noexcept
{
    if (!Admitted(installation)) return false;
    if (g_attempted.exchange(true, std::memory_order_acq_rel))
        return g_installed.load(std::memory_order_acquire);
    try {
        ArmorInstallation installer(installation.facts);
        return installer.Install(installation.trampolineInterface, installation.pluginHandle);
    } catch (...) { return false; }
}
}
