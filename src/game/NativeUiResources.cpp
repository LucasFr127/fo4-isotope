// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "NativeUiResources.hpp"
#include "DirectCallPatch.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/runtime/RuntimeProfile.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <span>
#include <tuple>

namespace {
namespace patch = fo4hdr::game::direct_call_patch;
namespace policy = fo4hdr::game::detail;
using Properties = RE::BSGraphics::RenderTargetProperties;
using Manager = RE::BSGraphics::RenderTargetManager;
using RegisterTarget = void (*)(Manager*, std::int32_t, const Properties*, std::uint8_t);
static_assert(sizeof(Properties) == 0x20);
static_assert(offsetof(Properties, format) == 8);
struct TrampolineInterface
{
    std::uint32_t version{}, padding{};
    void* (__cdecl* branch)(std::uint32_t, std::size_t){};
    void* (__cdecl* local)(std::uint32_t, std::size_t){};
};
static_assert(sizeof(TrampolineInterface) == 0x18);
static_assert(offsetof(TrampolineInterface, branch) == 8);
std::atomic<bool> g_attempted{}, g_installed{}, g_overlap{};
std::atomic<RegisterTarget> g_next{};
std::atomic_flag g_registration = ATOMIC_FLAG_INIT;
policy::NativeUiResourcePair g_pair;
std::uintptr_t g_managerAddress{}, g_rendererStorage{};

bool Read(const void* source, void* destination, std::size_t size) noexcept
{
    SIZE_T copied{};
    return source && ::ReadProcessMemory(::GetCurrentProcess(), source,
        destination, size, &copied) && copied == size;
}
bool ContainsBytes(std::uintptr_t base, std::size_t extent, std::uintptr_t address, std::size_t size) noexcept
{ return address >= base && address - base < extent && size <= extent - (address - base); }
bool Region(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& info) noexcept
{
    if (!address || !size) return false;
    if (!::VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return ContainsBytes(reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize,
        reinterpret_cast<std::uintptr_t>(address), size);
}
bool Executable(const void* address, std::size_t size, const void* module = nullptr) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!Region(address, size, info)) return false;
    if (module && info.AllocationBase != module) return false;
    if (module && info.Type != MEM_IMAGE) return false;
    const auto protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
}
bool MatchesBytes(std::uintptr_t address, std::span<const std::uint8_t> expected) noexcept
{
    std::array<std::uint8_t, 32> actual{};
    return expected.size() <= actual.size() && Read(reinterpret_cast<void*>(address),
        actual.data(), expected.size()) && std::equal(expected.begin(), expected.end(), actual.begin());
}
bool WriteBytes(std::uintptr_t address, std::span<const std::uint8_t> bytes, DWORD protection) noexcept
{
    DWORD prior{};
    if (!::VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), PAGE_EXECUTE_READWRITE, &prior))
        return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
    DWORD ignored{};
    const auto restored = ::VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), protection, &ignored);
    const auto flushed = ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size());
    MEMORY_BASIC_INFORMATION info{};
    return restored && flushed && Region(reinterpret_cast<void*>(address), bytes.size(), info) &&
        info.Protect == protection && MatchesBytes(address, bytes);
}
struct PatchBackend
{
    bool Matches(std::uintptr_t address, const patch::CallBytes& bytes) const noexcept
    { return MatchesBytes(address, bytes); }
    bool Replace(const patch::Site& site, const patch::CallBytes& before,
        const patch::CallBytes& after) const noexcept
    { return Matches(site.address, before) && WriteBytes(site.address, after, site.protection); }
};

Microsoft::WRL::ComPtr<IUnknown> ResourceIdentity(IUnknown* resource)
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (resource) (void)resource->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()));
    return identity;
}
bool SameResource(ID3D11View* view, ID3D11Texture2D* texture)
{
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    const auto resourceIdentity = ResourceIdentity(resource.Get());
    const auto textureIdentity = ResourceIdentity(texture);
    return resourceIdentity && textureIdentity && resourceIdentity.Get() == textureIdentity.Get();
}
bool NativeTargetPresent(const RE::BSGraphics::RenderTarget& target) noexcept
{
    return target.texture && target.rtView && target.srView && target.uaView &&
        !target.copyTexture && !target.copySRView;
}
bool ViewDescriptionsMatch(const RE::BSGraphics::RenderTarget& target)
{
    auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(target.rtView);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(target.srView);
    auto* uav = reinterpret_cast<ID3D11UnorderedAccessView*>(target.uaView);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    rtv->GetDesc(&rd); srv->GetDesc(&sd); uav->GetDesc(&ud);
    return std::tuple{rd.Format, sd.Format, ud.Format, rd.ViewDimension, sd.ViewDimension, ud.ViewDimension,
               rd.Texture2D.MipSlice, sd.Texture2D.MostDetailedMip, sd.Texture2D.MipLevels, ud.Texture2D.MipSlice} ==
        std::tuple{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_RTV_DIMENSION_TEXTURE2D, D3D11_SRV_DIMENSION_TEXTURE2D, D3D11_UAV_DIMENSION_TEXTURE2D,
            0u, 0u, 1u, 0u};
}
bool ViewsMatch(const RE::BSGraphics::RenderTarget& target)
{
    if (!NativeTargetPresent(target)) return false;
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(target.texture);
    return ViewDescriptionsMatch(target) &&
        SameResource(reinterpret_cast<ID3D11RenderTargetView*>(target.rtView), texture) &&
        SameResource(reinterpret_cast<ID3D11ShaderResourceView*>(target.srView), texture) &&
        SameResource(reinterpret_cast<ID3D11UnorderedAccessView*>(target.uaView), texture);
}
bool RecordedSlotMatches(Manager* manager, std::int32_t slot, const Properties& expected) noexcept
{
    Properties recorded{};
    if (!Read(&manager->renderTargetData[slot], &recorded, sizeof(recorded))) return false;
    Properties promoted;
    std::memcpy(&promoted, &expected, sizeof(promoted));
    promoted.format = static_cast<decltype(promoted.format)>(10);
    return std::memcmp(&recorded, &promoted, sizeof(recorded)) == 0;
}
bool ReadNativeTarget(Manager* manager, std::int32_t slot, RE::BSGraphics::RenderTarget& target) noexcept
{
    std::uint32_t platform{};
    if (!Read(&manager->GetRenderTargetPlatformID(slot), &platform, sizeof(platform))) return false;
    RE::BSGraphics::RendererData* renderer{};
    if (!Read(reinterpret_cast<void*>(g_rendererStorage), &renderer, sizeof(renderer))) return false;
    if (!renderer) return false;
    if (platform >= std::size(renderer->renderTargets)) return false;
    return Read(&renderer->renderTargets[platform], &target, sizeof(target));
}
bool TextureMatches(const D3D11_TEXTURE2D_DESC& desc, const Properties& expected) noexcept
{
    return std::tuple{desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, desc.Format,
               desc.SampleDesc.Count, desc.SampleDesc.Quality, desc.Usage, desc.BindFlags,
               desc.CPUAccessFlags, desc.MiscFlags} ==
        std::tuple{expected.width, expected.height, 1u, 1u, DXGI_FORMAT_R16G16B16A16_FLOAT,
            1u, 0u, D3D11_USAGE_DEFAULT,
            UINT{D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS}, 0u, 0u};
}
bool DedicatedSlotReady(Manager* manager, std::int32_t slot, const Properties& expected)
{
    if (!RecordedSlotMatches(manager, slot, expected)) return false;
    RE::BSGraphics::RenderTarget target{};
    if (!ReadNativeTarget(manager, slot, target) || !ViewsMatch(target)) return false;
    D3D11_TEXTURE2D_DESC desc{};
    reinterpret_cast<ID3D11Texture2D*>(target.texture)->GetDesc(&desc);
    return TextureMatches(desc, expected);
}
bool DedicatedPairDistinct(Manager* manager)
{
    RE::BSGraphics::RenderTarget first{}, second{};
    if (!ReadNativeTarget(manager, 24, first) || !ReadNativeTarget(manager, 25, second)) return false;
    const auto firstIdentity = ResourceIdentity(reinterpret_cast<ID3D11Texture2D*>(first.texture));
    const auto secondIdentity = ResourceIdentity(reinterpret_cast<ID3D11Texture2D*>(second.texture));
    return firstIdentity && secondIdentity && firstIdentity.Get() != secondIdentity.Get();
}
bool RegistrationQualified(Manager* manager, std::int32_t slot, std::uint8_t mode,
    const Properties& properties) noexcept
{
    return g_installed.load(std::memory_order_acquire) && (slot == 24 || slot == 25) && mode == 1 &&
        reinterpret_cast<std::uintptr_t>(manager) == g_managerAddress && policy::NativePauseProperties(properties);
}
void CompleteRegistration(const policy::NativeUiRegistration& call, const Properties* properties,
    bool paired, bool promoted)
{
    auto* manager = reinterpret_cast<Manager*>(call.manager);
    const auto valid = paired && promoted && DedicatedSlotReady(manager, call.slot, *properties);
    const auto complete = g_pair.Complete(call.slot, valid);
    const auto ready = complete && DedicatedSlotReady(manager, 24, *properties) && DedicatedPairDistinct(manager);
    fo4hdr::SetSharedUiPauseResourcesReady(ready);
}
struct RegistrationEntry
{
    bool owned{!g_registration.test_and_set(std::memory_order_acquire)};
    RegistrationEntry() noexcept
    {
        // Overlap is outside the proven native lifetime. Keep this fault sticky;
        // a later registration cannot erase another invocation's rejection.
        if (!owned) {
            g_overlap.store(true, std::memory_order_release);
            fo4hdr::DisableSharedUiPauseResources();
        }
        fo4hdr::SetSharedUiPauseResourcesReady(false);
    }
    ~RegistrationEntry() { if (owned) g_registration.clear(std::memory_order_release); }
};
void ForwardRegistration(const policy::NativeUiRegistration& call, const Properties* properties,
    RegisterTarget next, bool ownsPair)
{
    auto* manager = reinterpret_cast<Manager*>(call.manager);
    Properties observed{};
    const auto qualified = ownsPair && !g_overlap.load(std::memory_order_acquire) &&
        Read(properties, &observed, sizeof(observed)) &&
        RegistrationQualified(manager, call.slot, call.mode, observed);
    const auto paired = ownsPair && g_pair.Begin(call);
    const auto promoted = policy::ForwardNativeUiRegistration(qualified ? &observed : properties, qualified,
        [&](const Properties* forwarded) { next(manager, call.slot, forwarded, call.mode); });
    if (ownsPair) CompleteRegistration(call, &observed, paired, promoted);
}
void RegisterTargetHook(Manager* manager, std::int32_t slot, const Properties* properties, std::uint8_t mode) noexcept
{
    const auto next = g_next.load(std::memory_order_acquire);
    if (!next || next == &RegisterTargetHook) return;
    RegistrationEntry entry;
    try {
        const policy::NativeUiRegistration call{reinterpret_cast<std::uintptr_t>(manager),
            ::GetCurrentThreadId(), slot, mode};
        ForwardRegistration(call, properties, next, entry.owned);
    } catch (...) {
        // The native invocation may have consumed allocator bookkeeping. Never retry it.
        if (entry.owned) (void)g_pair.Complete(slot, false);
    }
}

class NativeUiInstallation
{
    const fo4hdr::NativeUiRuntimeProfile& identity_;
    REX::FModule module_{REX::FModule::GetExecutingModule()};
    std::uintptr_t base_{module_.GetBaseAddress()};
    REX::FModuleSection text_{};
    REL::Offset2ID mappings_;
    TrampolineInterface allocator_{};
    std::array<patch::Site, 2> sites_{};
    std::uintptr_t target_{};
    std::uintptr_t hook_{reinterpret_cast<std::uintptr_t>(&RegisterTargetHook)};
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
        return target_ != hook_ && Mapping(identity_.targetId, identity_.targetRva) &&
            Code(target_, identity_.targetBytes.size()) && MatchesBytes(target_, identity_.targetBytes);
    }
    bool Allocator(const void* allocatorInterface, std::uint32_t handle)
    {
        return Read(allocatorInterface, &allocator_, sizeof(allocator_)) && allocator_.version == 1 &&
            handle != UINT32_MAX && Executable(reinterpret_cast<void*>(allocator_.branch), 1);
    }
    bool SiteMatches(std::size_t index) const
    {
        const auto address = base_ + identity_.callRvas[index];
        return Mapping(identity_.callerIds[index], identity_.callerRvas[index]) &&
            Code(base_ + identity_.callerRvas[index], 1) && Code(address, 5) &&
            MatchesBytes(address, identity_.callBytes[index]) &&
            patch::CallTarget(address, identity_.callBytes[index]) == target_;
    }
    bool PrepareSite(std::size_t index)
    {
        const auto address = base_ + identity_.callRvas[index];
        MEMORY_BASIC_INFORMATION info{};
        if (!SiteMatches(index) || !Region(reinterpret_cast<void*>(address), 5, info)) return false;
        sites_[index] = {address, identity_.callBytes[index], {}, info.Protect};
        return true;
    }
    bool PrepareRelay(patch::Site& site, std::uint32_t handle)
    {
        auto* relay = allocator_.branch(handle, 14);
        MEMORY_BASIC_INFORMATION info{};
        if (!Executable(relay, 14) || !Region(relay, 14, info)) return false;
        if (!patch::EncodeCall(site.address, reinterpret_cast<std::uintptr_t>(relay), site.replacement)) return false;
        std::array<std::uint8_t, 14> bytes{0xFF, 0x25, 0, 0, 0, 0};
        std::memcpy(bytes.data() + 6, &hook_, sizeof(hook_));
        return WriteBytes(reinterpret_cast<std::uintptr_t>(relay), bytes, info.Protect);
    }
    bool PrepareSites(std::uint32_t handle)
    {
        for (std::size_t index = 0; index < sites_.size(); ++index)
            if (!PrepareSite(index)) return false;
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
        // Failed rollback may leave a relay reachable. Its exact native predecessor
        // must remain callable even when admission was never published.
        g_next.store(reinterpret_cast<RegisterTarget>(target_), std::memory_order_release);
        PatchBackend backend;
        if (!patch::InstallPair(sites_, backend)) return false;
        g_installed.store(true, std::memory_order_release);
        return true;
    }
    bool Globals()
    {
        const auto data = module_.GetSection(".data");
        g_managerAddress = reinterpret_cast<std::uintptr_t>(Manager::GetSingleton());
        g_rendererStorage = RE::ID::BSGraphics::GetRendererData.address();
        return ContainsBytes(data.GetAddress(), data.GetSize(), g_managerAddress, sizeof(Manager)) &&
            ContainsBytes(data.GetAddress(), data.GetSize(), g_rendererStorage, sizeof(void*));
    }
public:
    explicit NativeUiInstallation(const fo4hdr::NativeUiRuntimeProfile& identity) :
        identity_(identity), target_(base_ + identity.targetRva) {}

    bool Install(const void* allocatorInterface, std::uint32_t handle)
    {
        const auto* profile = fo4hdr::ActiveRuntimeProfile();
        if (!base_ || !profile) return false;
        if (module_.GetFileVersion().pack() != profile->runtimeVersion) return false;
        text_ = module_.GetSection(".text");
        mappings_.load_v2();
        return Target() && Globals() && Allocator(allocatorInterface, handle) && PrepareSites(handle) && Publish();
    }
};
}  // namespace

namespace fo4hdr::game {
bool InitializeNativeUiResources(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept
{
    if (g_attempted.exchange(true, std::memory_order_acq_rel))
        return g_installed.load(std::memory_order_acquire);
    fo4hdr::SetSharedUiPauseResourcesReady(false);
    if (!requested) return false;
    const auto* profile = ActiveRuntimeProfile();
    if (!profile || !profile->nativeUi) return false;
    try {
        NativeUiInstallation installation(*profile->nativeUi);
        return installation.Install(trampolineInterface, pluginHandle);
    } catch (...) { return false; }
}
}  // namespace fo4hdr::game
