// SPDX-License-Identifier: GPL-3.0-or-later

#include "DeviceHooks.hpp"
#include "HookRecords.hpp"
#include "NativeImageSlots.hpp"
#include "SwapChainHooks.hpp"
#include "../resources/NativeSceneAssociation.hpp"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace fo4hdr {

std::atomic<DeviceHookRecord*> g_deviceHooks{};
std::atomic<ContextHookRecord*> g_contextHooks{};
std::atomic<SwapChainHookRecord*> g_swapChainHooks{};

namespace {

using Microsoft::WRL::ComPtr;
constexpr std::array<std::size_t, 5> deviceSlots{5, 7, 8, 9, 15};
constexpr std::array<std::size_t, 10> contextSlots{9, 12, 13, 20, 21, 27, 28, 33, 34, 58};
constexpr std::array<std::size_t, 2> chainSlots{8, 13};
constexpr std::size_t maximumFileBytes = 64 * 1024 * 1024;
DeviceHookRecord residentDevice;
SwapChainHookRecord residentChain;
alignas(ContextHookRecord) std::array<std::byte, sizeof(ContextHookRecord)> residentContextStorage;
std::atomic_flag residentClaim = ATOMIC_FLAG_INIT;

class ImageFile
{
public:
    ImageFile() noexcept = default;
    ~ImageFile() noexcept
    {
        if (data_ != nullptr) UnmapViewOfFile(data_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    ImageFile(const ImageFile&) = delete;
    ImageFile& operator=(const ImageFile&) = delete;

    bool Open(const wchar_t* path) noexcept
    {
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size)) return false;
        if (size.QuadPart <= 0 || size.QuadPart > maximumFileBytes) return false;
        bytes_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) return false;
        data_ = static_cast<const std::byte*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, bytes_));
        return data_ != nullptr;
    }

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {data_, bytes_}; }

private:
    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mapping_{};
    const std::byte* data_{};
    std::size_t bytes_{};
};

bool ImageRange(const void* address, std::size_t bytes, HMODULE module) noexcept
{
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(address, &page, sizeof(page)) != sizeof(page)) return false;
    if (page.State != MEM_COMMIT || page.Type != MEM_IMAGE) return false;
    if (page.AllocationBase != module || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    if (start < base || start - base > page.RegionSize) return false;
    return bytes <= page.RegionSize - (start - base);
}

bool DataSlot(void** slot, HMODULE module) noexcept
{
    if (reinterpret_cast<std::uintptr_t>(slot) % std::atomic_ref<void*>::required_alignment != 0)
        return false;
    if (!ImageRange(slot, sizeof(void*), module)) return false;
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(slot, &page, sizeof(page)) != sizeof(page)) return false;
    constexpr std::array<DWORD, 3> readable{PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY};
    return std::find(readable.begin(), readable.end(), page.Protect & 0xFF) != readable.end();
}

bool SystemImagePath(HMODULE module, std::wstring_view filename,
    std::array<wchar_t, 32768>& path) noexcept
{
    const auto loaded = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (loaded == 0 || loaded >= path.size()) return false;
    std::array<wchar_t, 32768> expected{};
    const auto directory = GetSystemDirectoryW(expected.data(), static_cast<UINT>(expected.size()));
    if (directory == 0 || directory + filename.size() + 2 > expected.size()) return false;
    expected[directory] = L'\\';
    std::copy(filename.begin(), filename.end(), expected.begin() + directory + 1);
    return CompareStringOrdinal(path.data(), -1, expected.data(), -1, TRUE) == CSTR_EQUAL;
}

bool ReadableImageRange(const void* address, std::size_t bytes, HMODULE module) noexcept
{
    if (!ImageRange(address, bytes, module)) return false;
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(address, &page, sizeof(page)) != sizeof(page)) return false;
    constexpr std::array<DWORD, 6> readable{PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY,
        PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY};
    return std::find(readable.begin(), readable.end(), page.Protect & 0xFF) != readable.end();
}

bool LoadedHeaderMatches(HMODULE module, const NativeImageSlots& disk) noexcept
{
    const auto headers = disk.HeaderBytes();
    if (headers.empty()) return false;
    if (!ReadableImageRange(module, headers.size(), module)) return false;
    // This intentionally refuses loader-modified headers instead of guessing layout.
    return std::memcmp(module, headers.data(), headers.size()) == 0;
}

bool PinCode(void* address, HMODULE expected = nullptr) noexcept
{
    HMODULE pinned{};
    const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &pinned)) return false;
    return expected == nullptr || pinned == expected;
}

class NativeSlotProof
{
public:
    bool Prepare(void* creationNext) noexcept
    {
        module_ = GetModuleHandleW(L"d3d11.dll");
        if (module_ == nullptr) return false;
        if (creationNext != reinterpret_cast<void*>(GetProcAddress(module_, "D3D11CreateDeviceAndSwapChain")))
            return false;
        if (!PrepareImage(L"d3d11.dll")) return false;
        if (!ImageRange(creationNext, 1, module_)) return false;
        if (!IsExecutableHookTarget(creationNext)) return false;
        if (!PinCode(creationNext, module_)) return false;
        return PinCode(reinterpret_cast<void*>(&HookBegin));
    }

    bool PrepareChain() noexcept
    {
        module_ = GetModuleHandleW(L"dxgi.dll");
        if (module_ == nullptr) return false;
        if (!PrepareImage(L"dxgi.dll")) return false;
        return PinCode(reinterpret_cast<void*>(module_), module_);
    }

    bool PrepareImage(std::wstring_view filename) noexcept
    {
        std::array<wchar_t, 32768> path{};
        if (!SystemImagePath(module_, filename, path)) return false;
        if (!file_.Open(path.data())) return false;
        if (!disk_.Parse(file_.Bytes())) return false;
        return LoadedHeaderMatches(module_, disk_);
    }

    void* Expected(void** slot) const noexcept
    {
        if (!DataSlot(slot, module_)) return nullptr;
        const auto base = reinterpret_cast<std::uintptr_t>(module_);
        const auto address = reinterpret_cast<std::uintptr_t>(slot);
        if (address < base || address - base >= disk_.Header().OptionalHeader.SizeOfImage) return nullptr;
        const auto expected = disk_.Expected(base, static_cast<std::uint32_t>(address - base));
        if (expected == nullptr) return nullptr;
        if (!ImageRange(expected, 1, module_)) return nullptr;
        if (!IsExecutableHookTarget(expected)) return nullptr;
        return AtomicPointerLoad(slot) == expected ? expected : nullptr;
    }

private:
    HMODULE module_{};
    ImageFile file_;
    NativeImageSlots disk_;
};

template <class Alias, class Base>
bool SameSdkAlias(Base* object) noexcept
{
    ComPtr<Alias> alias;
    const auto result = object->QueryInterface(IID_PPV_ARGS(&alias));
    if (result == E_NOINTERFACE) return true;
    if (result != S_OK || !alias) return false;
    if (reinterpret_cast<void*>(alias.Get()) != reinterpret_cast<void*>(object)) return false;
    return *reinterpret_cast<void***>(alias.Get()) == *reinterpret_cast<void***>(object);
}

template <class... Aliases, class Base>
bool SameSdkAliases(Base* object) noexcept
{
    return (SameSdkAlias<Aliases>(object) && ...);
}

bool AdmittedAliases(const CreatedDeviceBinding& created) noexcept
{
    return SameSdkAliases<ID3D11Device1, ID3D11Device2, ID3D11Device3,
        ID3D11Device4, ID3D11Device5>(created.device) &&
        SameSdkAliases<ID3D11DeviceContext1, ID3D11DeviceContext2,
            ID3D11DeviceContext3, ID3D11DeviceContext4>(created.context);
}

bool AdmittedChain(const CreatedDeviceBinding& created, DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    if (created.swapChain == nullptr) return false;
    if (!SameSdkAliases<IDXGISwapChain1, IDXGISwapChain2, IDXGISwapChain3,
            IDXGISwapChain4>(created.swapChain)) return false;
    if (FAILED(created.swapChain->GetDesc(&description))) return false;
    return IsWindow(description.OutputWindow) && description.BufferDesc.Width != 0 &&
        description.BufferDesc.Height != 0;
}

template <std::size_t Count>
bool CaptureOriginals(NativeSlotProof& proof, void** table,
    const std::array<std::size_t, Count>& slots, std::array<void*, Count>& originals) noexcept
{
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        originals[index] = proof.Expected(table + slots[index]);
        if (originals[index] == nullptr) return false;
    }
    return true;
}

void PublishDeviceNexts(const std::array<void*, 5>& originals) noexcept
{
    residentDevice.nextCreateTexture2D = reinterpret_cast<CreateTexture2D>(originals[0]);
    residentDevice.nextCreateSrv = reinterpret_cast<CreateShaderResourceView>(originals[1]);
    residentDevice.nextCreateUav = reinterpret_cast<CreateUnorderedAccessView>(originals[2]);
    residentDevice.nextCreateRtv = reinterpret_cast<CreateRenderTargetView>(originals[3]);
    residentDevice.nextCreatePixelShader = reinterpret_cast<CreatePixelShader>(originals[4]);
}

void PublishContextNexts(ContextHookRecord& record, const std::array<void*, 10>& originals) noexcept
{
    record.nextPsSetShader = reinterpret_cast<PsSetShader>(originals[0]);
    record.nextDrawIndexed = reinterpret_cast<DrawIndexed>(originals[1]);
    record.nextDraw = reinterpret_cast<Draw>(originals[2]);
    record.nextDrawIndexedInstanced = reinterpret_cast<DrawIndexedInstanced>(originals[3]);
    record.nextDrawInstanced = reinterpret_cast<DrawInstanced>(originals[4]);
    record.nextBegin = reinterpret_cast<ContextQueryCall>(originals[5]);
    record.nextEnd = reinterpret_cast<ContextQueryCall>(originals[6]);
    record.nextOmSetRenderTargets = reinterpret_cast<OmSetRenderTargets>(originals[7]);
    record.nextOmSetRenderTargetsAndUavs = reinterpret_cast<OmSetRenderTargetsAndUavs>(originals[8]);
    record.nextExecuteCommandList = reinterpret_cast<ContextCommandListCall>(originals[9]);
}

template <auto Next>
bool PublishDeviceOriginal(void* next) noexcept
{
    const auto* record = g_deviceHooks.load(std::memory_order_acquire);
    return next == reinterpret_cast<void*>(record->*Next);
}

template <auto Next>
bool PublishContextOriginal(void* next) noexcept
{
    const auto* record = g_contextHooks.load(std::memory_order_acquire);
    return next == reinterpret_cast<void*>(record->*Next);
}

template <auto Next>
bool PublishChainOriginal(void* next) noexcept
{
    const auto* record = g_swapChainHooks.load(std::memory_order_acquire);
    return next == reinterpret_cast<void*>(record->*Next);
}

struct SlotPatch
{
    std::size_t index;
    void* replacement;
    HookNextPublisher publish;
};

const std::array devicePatches{
    SlotPatch{5, reinterpret_cast<void*>(&HookCreateTexture2D), PublishDeviceOriginal<&DeviceHookRecord::nextCreateTexture2D>},
    SlotPatch{7, reinterpret_cast<void*>(&HookCreateSrv), PublishDeviceOriginal<&DeviceHookRecord::nextCreateSrv>},
    SlotPatch{8, reinterpret_cast<void*>(&HookCreateUav), PublishDeviceOriginal<&DeviceHookRecord::nextCreateUav>},
    SlotPatch{9, reinterpret_cast<void*>(&HookCreateRtv), PublishDeviceOriginal<&DeviceHookRecord::nextCreateRtv>},
    SlotPatch{15, reinterpret_cast<void*>(&HookCreatePixelShader), PublishDeviceOriginal<&DeviceHookRecord::nextCreatePixelShader>}};
const std::array contextPatches{
    SlotPatch{9, reinterpret_cast<void*>(&HookPsSetShader), PublishContextOriginal<&ContextHookRecord::nextPsSetShader>},
    SlotPatch{12, reinterpret_cast<void*>(&HookDrawIndexed), PublishContextOriginal<&ContextHookRecord::nextDrawIndexed>},
    SlotPatch{13, reinterpret_cast<void*>(&HookDraw), PublishContextOriginal<&ContextHookRecord::nextDraw>},
    SlotPatch{20, reinterpret_cast<void*>(&HookDrawIndexedInstanced), PublishContextOriginal<&ContextHookRecord::nextDrawIndexedInstanced>},
    SlotPatch{21, reinterpret_cast<void*>(&HookDrawInstanced), PublishContextOriginal<&ContextHookRecord::nextDrawInstanced>},
    SlotPatch{27, reinterpret_cast<void*>(&HookBegin), PublishContextOriginal<&ContextHookRecord::nextBegin>},
    SlotPatch{28, reinterpret_cast<void*>(&HookEnd), PublishContextOriginal<&ContextHookRecord::nextEnd>},
    SlotPatch{33, reinterpret_cast<void*>(&HookOmSetRenderTargets), PublishContextOriginal<&ContextHookRecord::nextOmSetRenderTargets>},
    SlotPatch{34, reinterpret_cast<void*>(&HookOmSetRenderTargetsAndUavs), PublishContextOriginal<&ContextHookRecord::nextOmSetRenderTargetsAndUavs>},
    SlotPatch{58, reinterpret_cast<void*>(&HookExecuteCommandList), PublishContextOriginal<&ContextHookRecord::nextExecuteCommandList>}};
const std::array chainPatches{
    SlotPatch{8, reinterpret_cast<void*>(&HookPresent), PublishChainOriginal<&SwapChainHookRecord::nextPresent>},
    SlotPatch{13, reinterpret_cast<void*>(&HookResizeBuffers), PublishChainOriginal<&SwapChainHookRecord::nextResize>}};

template <class Record, std::size_t Count>
bool CaptureSlotProtection(Record& record, const std::array<SlotPatch, Count>& patches) noexcept
{
    for (std::size_t index = 0; index < Count; ++index)
    {
        MEMORY_BASIC_INFORMATION page{};
        if (VirtualQuery(record.table + patches[index].index, &page, sizeof(page)) != sizeof(page)) return false;
        record.slotProtection[index] = page.Protect;
    }
    return true;
}

template <class Record, std::size_t Count>
bool InstallSlots(Record& record, const std::array<SlotPatch, Count>& patches) noexcept
{
    const auto writer = static_cast<HookPointerWriter>(&WriteProtectedPointer);
    for (std::size_t index = 0; index < patches.size(); ++index)
        if (!record.owners[index].Install(record.table + patches[index].index,
                patches[index].replacement, writer, patches[index].publish)) return false;
    return true;
}

template <class Record, std::size_t Count>
void RollbackSlots(Record& record, const std::array<SlotPatch, Count>& patches) noexcept
{
    const auto writer = static_cast<HookPointerWriter>(&WriteProtectedPointer);
    for (std::size_t index = patches.size(); index != 0; --index)
    {
        auto& owner = record.owners[index - 1];
        if (owner.State() == HookOwnershipState::owned) static_cast<void>(owner.Remove(writer));
        else if (owner.State() == HookOwnershipState::passive)
            static_cast<void>(writer(record.table + patches[index - 1].index,
                patches[index - 1].replacement, owner.Next()));
    }
}

bool QualifiedSettingsStartup(const NativeSettingsStartup& startup) noexcept
{
    if (!startup.prepared) return false;
    const auto& value = startup.configuration.settings;
    if (!ValidatePluginConfiguration(value)) return false;
    const auto& result = *startup.prepared;
    if (result.request.algorithm != value.algorithm ||
        result.request.displayPeakNits != value.display.displayPeakNits) return false;
    return ActiveToneMapperParameters(value.algorithm, value.display.toneMapperParameters) ==
        ActiveToneMapperParameters(result.request.algorithm, result.request.parameters) &&
        PreparedOperatorLutMatches(result.data, result.request);
}

ScenePassConfiguration InitialSceneConfiguration(const DXGI_SWAP_CHAIN_DESC& description,
    const NativeSettingsStartup* startup) noexcept
{
    ScenePassConfiguration configuration{description.BufferDesc.Width, description.BufferDesc.Height};
    if (startup == nullptr) return configuration;
    const auto& display = startup->configuration.settings.display;
    configuration.whites = {static_cast<float>(display.uiPaperWhiteNits),
        static_cast<float>(display.videoWhiteNits)};
    configuration.operatorLut = &startup->prepared->data;
    configuration.sceneGrade = {static_cast<float>(display.vanillaColorGradingStrengthPercent / 100.0),
        DeriveHdrSettingsMath(display), display.colorGradingMethod, display.selectiveColor};
    return configuration;
}

bool PrepareHdrOutput(ContextHookRecord& record, const CreatedDeviceBinding& created,
    ScenePassConfiguration configuration) noexcept
{
    if (FAILED(record.renderer.AdoptPresentationScene(record.presentationProxy, record.presentationScene))) return false;
    const PresentationConfiguration output{configuration.width, configuration.height,
        static_cast<float>(configuration.operatorLut->displayPeakNits), &record.renderer,
        configuration.whites, record.settingsHost.Window()};
    if (!record.presentation.Initialize(created.device, created.context, output)) return false;
    if (FAILED(record.presentation.PrepareNativeChain(created.swapChain))) return false;
    record.hdrPrepared = true;
    return true;
}

bool PrepareRenderer(ContextHookRecord& record, const CreatedDeviceBinding& created,
    const NativeSettingsStartup* startup) noexcept
{
    if (created.swapChain == nullptr) return false;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(created.swapChain->GetDesc(&description))) return false;
    const auto configuration = InitialSceneConfiguration(description, startup);
    if (!record.renderer.InitializeBorrowed(created.device, created.context, configuration)) return false;
    const bool ui = record.uiCapture.Initialize(created.device, created.context, configuration);
    const bool proxy = record.presentationProxy.Prepare(created.device, created.context, created.swapChain);
    if (startup != nullptr)
    {
        if (!ui || !proxy) return false;
        if (!PrepareHdrOutput(record, created, configuration)) return false;
    }
    const ContextQueryPredecessors next{record.nextBegin, record.nextEnd, record.nextExecuteCommandList};
    return record.queries.Prepare(created.context, record.generation, next, &record.renderer) ==
        ContextQueryPreparation::coverageUnobserved;
}

template <class Call, class... Arguments>
HRESULT NativeResult(Call next, Arguments... arguments) noexcept
{
    try { return next(arguments...); }
    catch (...) { return E_FAIL; }
}

bool CurrentNativeSceneContext(ContextHookRecord& context) noexcept
{
    return NativeRoutingOwned() && context.queries.CanDispatch();
}

bool NativeSceneDimensions(const DXGI_SWAP_CHAIN_DESC& actual,
    const DXGI_SWAP_CHAIN_DESC& expected) noexcept
{
    const std::array dimensions{actual.BufferDesc.Width, actual.BufferDesc.Height};
    const std::array owned{expected.BufferDesc.Width, expected.BufferDesc.Height};
    return actual.BufferDesc.Width != 0 && actual.BufferDesc.Height != 0 && dimensions == owned;
}

bool NativeSceneAdmission(ContextHookRecord& context) noexcept
{
    const auto* chain = g_swapChainHooks.load(std::memory_order_acquire);
    const auto* device = g_deviceHooks.load(std::memory_order_acquire);
    if (chain == nullptr) return false;
    if (device == nullptr) return false;
    if (!chain->resourcesReady.load(std::memory_order_acquire)) return false;
    if (!CurrentNativeSceneContext(context)) return false;
    if (!context.renderer.MatchesContext(device->owned, context.owned)) return false;
    DXGI_SWAP_CHAIN_DESC actual{};
    if (FAILED(chain->owned->GetDesc(&actual))) return false;
    return NativeSceneDimensions(actual, chain->description) && CurrentNativeSceneContext(context);
}

struct PrivateNativeScene
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
};

HRESULT PrepareNativeViews(const DeviceHookRecord& device, PrivateNativeScene& scene) noexcept
{
    auto result = NativeResult(device.nextCreateSrv, device.owned,
        scene.texture.Get(), nullptr, scene.source.GetAddressOf());
    if (result != S_OK) return FAILED(result) ? result : E_FAIL;
    if (!scene.source) return E_FAIL;
    result = NativeResult(device.nextCreateRtv, device.owned,
        scene.texture.Get(), nullptr, scene.target.GetAddressOf());
    if (result != S_OK) return FAILED(result) ? result : E_FAIL;
    return scene.target ? S_OK : E_FAIL;
}

HRESULT CreatePromotedScene(const DeviceHookRecord& device, ContextHookRecord& context,
    const D3D11_TEXTURE2D_DESC& original, ID3D11Texture2D** output) noexcept
{
    *output = nullptr;
    auto promoted = original;
    promoted.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    PrivateNativeScene scene;
    auto result = NativeResult(device.nextCreateTexture2D, device.owned,
        &promoted, nullptr, scene.texture.GetAddressOf());
    if (result != S_OK) return FAILED(result) ? result : E_FAIL;
    if (!scene.texture) return E_FAIL;
    result = PrepareNativeViews(device, scene);
    if (FAILED(result)) return result;
    NativeSceneTexture candidate;
    const NativeSceneCreation creation{device.owned, original, context.generation};
    if (!candidate.Prepare(creation, {scene.texture.Get(), scene.source.Get(), scene.target.Get()}))
        return E_FAIL;
    if (!NativeSceneAdmission(context)) return E_FAIL;
    // Slot commit and output transfer perform no new native calls. No retry can
    // safely assume admission survived the first private creation transaction.
    SceneTextureHandle handle;
    result = context.renderer.AdoptPromotedNativeSceneTexture(candidate, handle);
    if (FAILED(result)) return result;
    *output = scene.texture.Detach();
    return S_OK;
}

const ContextHookRecord* AttributedDeviceOwner(ID3D11Device* device) noexcept
{
    const auto* owned = g_deviceHooks.load(std::memory_order_acquire);
    const auto* context = g_contextHooks.load(std::memory_order_acquire);
    if (owned == nullptr) return nullptr;
    if (context == nullptr || !context->retainedReferences.load(std::memory_order_acquire)) return nullptr;
    if (device != owned->owned) return nullptr;
    // Read compatibility attribution after retirement, but never inspect mutable
    // renderer slots or use device APIs from an unqualified foreign thread.
    return GetCurrentThreadId() == context->thread ? context : nullptr;
}

bool AttributedResource(ID3D11Device* device, ID3D11Resource* resource) noexcept
{
    const auto* context = AttributedDeviceOwner(device);
    if (context == nullptr) return false;
    if (resource == nullptr) return false;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
    NativeSceneAttribution record{};
    return ReadNativeSceneAttribution(device, texture.Get(), context->generation, record);
}

bool RemappableSrv(const D3D11_SHADER_RESOURCE_VIEW_DESC& value) noexcept
{
    return value.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
        value.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        value.Texture2D.MostDetailedMip == 0 &&
        (value.Texture2D.MipLevels == 1 || value.Texture2D.MipLevels == UINT(-1));
}

bool RemappableRtv(const D3D11_RENDER_TARGET_VIEW_DESC& value) noexcept
{
    return value.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
        value.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && value.Texture2D.MipSlice == 0;
}

bool CompatibleSrvRequest(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description) noexcept
{
    if (description == nullptr) return false;
    if (!RemappableSrv(*description)) return false;
    return AttributedResource(device, resource);
}

bool CompatibleRtvRequest(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* description) noexcept
{
    if (description == nullptr) return false;
    if (!RemappableRtv(*description)) return false;
    return AttributedResource(device, resource);
}

bool NativeTextureRequest(const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** texture) noexcept
{
    return description != nullptr && texture != nullptr && initialData == nullptr;
}

bool PromotableNativeRequest(ContextHookRecord* context, const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (context == nullptr) return false;
    return NativeSceneAdmission(*context) && context->renderer.CanPromoteNativeSceneTexture(description);
}

struct NativeBindingPreparation
{
    NativeSlotProof proof;
    NativeSlotProof chainProof;
    void** deviceTable{};
    void** contextTable{};
    void** chainTable{};
    std::array<void*, 5> deviceOriginals{};
    std::array<void*, 10> contextOriginals{};
    std::array<void*, 2> chainOriginals{};
    DXGI_SWAP_CHAIN_DESC description{};
    const NativeSettingsStartup* startup{};

    bool PrepareChain(const CreatedDeviceBinding& created) noexcept
    {
        if (!AdmittedChain(created, description)) return false;
        if (!chainProof.PrepareChain()) return false;
        chainTable = *reinterpret_cast<void***>(created.swapChain);
        return CaptureOriginals(chainProof, chainTable, chainSlots, chainOriginals);
    }

    bool Prepare(const CreatedDeviceBinding& created, void* creationNext) noexcept
    {
        if (!proof.Prepare(creationNext)) return false;
        if (!AdmittedAliases(created)) return false;
        deviceTable = *reinterpret_cast<void***>(created.device);
        contextTable = *reinterpret_cast<void***>(created.context);
        if (!CaptureOriginals(proof, deviceTable, deviceSlots, deviceOriginals)) return false;
        if (!CaptureOriginals(proof, contextTable, contextSlots, contextOriginals)) return false;
        return PrepareChain(created);
    }

    ContextHookRecord* Publish(const CreatedDeviceBinding& created, std::uint64_t generation) noexcept
    {
        auto* context = std::construct_at(reinterpret_cast<ContextHookRecord*>(residentContextStorage.data()));
        residentDevice.table = deviceTable;
        residentDevice.originals = deviceOriginals;
        context->table = contextTable;
        context->originals = contextOriginals;
        context->generation = generation;
        context->thread = GetCurrentThreadId();
        residentChain.table = chainTable;
        residentChain.originals = chainOriginals;
        residentChain.nextPresent = reinterpret_cast<SwapChainPresent>(chainOriginals[0]);
        residentChain.nextResize = reinterpret_cast<SwapChainResize>(chainOriginals[1]);
        residentChain.description = description;
        PublishDeviceNexts(deviceOriginals);
        PublishContextNexts(*context, contextOriginals);
        // Reachable wrappers retain these objects even after generation retirement.
        created.device->AddRef();
        created.context->AddRef();
        created.swapChain->AddRef();
        residentDevice.owned = created.device;
        context->owned = created.context;
        residentChain.owned = created.swapChain;
        g_deviceHooks.store(&residentDevice, std::memory_order_release);
        g_contextHooks.store(context, std::memory_order_release);
        context->retainedReferences.store(true, std::memory_order_release);
        g_swapChainHooks.store(&residentChain, std::memory_order_release);
        if (!CaptureSlotProtection(residentDevice, devicePatches)) return nullptr;
        if (!CaptureSlotProtection(*context, contextPatches)) return nullptr;
        if (!CaptureSlotProtection(residentChain, chainPatches)) return nullptr;
        return PrepareRenderer(*context, created, startup) ? context : nullptr;
    }
};

bool InstallNativeSlots(ContextHookRecord& context) noexcept
{
    if (!InstallSlots(residentDevice, devicePatches)) return false;
    if (!InstallSlots(context, contextPatches)) return false;
    return InstallSlots(residentChain, chainPatches);
}

bool RetireNativeOwners(ContextHookRecord& record) noexcept
{
    return record.settingsHost.RetireCold() && record.presentation.Retire() && record.uiCapture.Retire() &&
        record.presentationProxy.Retire() && record.renderer.Retire() && record.queries.Retire();
}

bool NativeRetirementSafe(const ContextHookRecord& record) noexcept
{
    return GetCurrentThreadId() == record.thread && !record.busy.test(std::memory_order_acquire) &&
        !residentChain.busy.test(std::memory_order_acquire) && record.queries.CanDispatch();
}

bool SlotProtectionMatches(void** slot, DWORD protection) noexcept
{
    MEMORY_BASIC_INFORMATION page{};
    return VirtualQuery(slot, &page, sizeof(page)) == sizeof(page) && page.Protect == protection;
}

template <class Record>
bool RestoreFreshSlot(Record& record, SlotPatch patch, std::size_t index) noexcept
{
    auto* slot = record.table + patch.index;
    if (!SlotProtectionMatches(slot, record.slotProtection[index])) return false;
    const auto current = AtomicPointerLoad(slot);
    if (current == record.originals[index]) return true;
    if (current != patch.replacement) return false;
    auto& owner = record.owners[index];
    const auto writer = static_cast<HookPointerWriter>(&WriteProtectedPointer);
    const bool changed = owner.State() == HookOwnershipState::owned
        ? owner.Remove(writer) : writer(slot, patch.replacement, record.originals[index]);
    return changed && AtomicPointerLoad(slot) == record.originals[index];
}

template <class Record, std::size_t Count>
bool RestoreFreshSlots(Record& record, const std::array<SlotPatch, Count>& patches) noexcept
{
    bool restored = true;
    for (std::size_t reverse = Count; reverse != 0; --reverse)
    {
        const auto index = reverse - 1;
        if (!RestoreFreshSlot(record, patches[index], index)) restored = false;
    }
    return restored;
}

bool MatchesFreshObjects(const CreatedDeviceBinding& created, const ContextHookRecord& record) noexcept
{
    return created.context == record.owned && created.device == residentDevice.owned &&
        created.swapChain == residentChain.owned;
}

bool RestoreFreshTables(ContextHookRecord& record) noexcept
{
    const bool chain = RestoreFreshSlots(residentChain, chainPatches);
    const bool context = RestoreFreshSlots(record, contextPatches);
    const bool device = RestoreFreshSlots(residentDevice, devicePatches);
    return chain && context && device;
}

void ReleaseFreshReferences(ContextHookRecord& record) noexcept
{
    if (!record.retainedReferences.exchange(false, std::memory_order_acq_rel)) return;
    residentChain.owned->Release();
    record.owned->Release();
    residentDevice.owned->Release();
}

bool FreshOwnerSafe(const ContextHookRecord& record) noexcept
{
    return GetCurrentThreadId() == record.thread && !record.busy.test(std::memory_order_acquire) &&
        !residentChain.busy.test(std::memory_order_acquire);
}

void WithdrawFailedInstallation(ContextHookRecord& context, bool hdr) noexcept
{
    RevokeNativeRouting();
    if (hdr) return;
    RollbackSlots(residentChain, chainPatches);
    RollbackSlots(context, contextPatches);
    RollbackSlots(residentDevice, devicePatches);
}

bool ValidCreation(const CreatedDeviceBinding& created, std::uint64_t generation) noexcept
{
    return created.device != nullptr && created.context != nullptr && generation != 0;
}

}  // namespace

bool NativeRoutingOwned() noexcept
{
    auto* device = g_deviceHooks.load(std::memory_order_acquire);
    auto* context = g_contextHooks.load(std::memory_order_acquire);
    if (device == nullptr || context == nullptr) return false;
    if (!context->retainedReferences.load(std::memory_order_acquire)) return false;
    if (*reinterpret_cast<void***>(device->owned) != device->table) return false;
    if (*reinterpret_cast<void***>(context->owned) != context->table) return false;
    const auto owned = [](auto& slot) { return slot.Health() == HookOwnershipState::owned; };
    return NativeChainOwned() && std::all_of(device->owners.begin(), device->owners.end(), owned) &&
        std::all_of(context->owners.begin(), context->owners.end(), owned);
}

void RevokeNativeRouting() noexcept
{
    auto* chain = g_swapChainHooks.load(std::memory_order_acquire);
    if (chain != nullptr)
    {
        chain->admitted.store(false, std::memory_order_release);
        chain->resourcesReady.store(false, std::memory_order_release);
    }
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return;
    record->admitted.store(false, std::memory_order_release);
    record->presentationProxy.RevokePublication();
    record->queries.RevokeCoverage();
    record->settingsHost.Revoke();
}

bool NativeRoutingInstaller::CompleteCoverage() noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr || !NativeRoutingOwned()) return false;
    if (record->busy.test(std::memory_order_acquire)) return false;
    if (!record->queries.CompleteFreshCoverage(record->owned, record->generation)) return false;
    record->admitted.store(true, std::memory_order_release);
    residentChain.resourcesReady.store(true, std::memory_order_release);
    residentChain.admitted.store(true, std::memory_order_release);
    return true;
}

NativeRoutingStatus NativeRoutingInstaller::Install(const CreatedDeviceBinding& created,
    std::uint64_t generation, void* creationNext) noexcept
{
    return InstallImpl(created, generation, creationNext, nullptr);
}

bool NativeRoutingInstaller::InstallHdr(const CreatedDeviceBinding& created,
    std::uint64_t generation, void* creationNext, const NativeSettingsStartup& startup) noexcept
{
    if (!QualifiedSettingsStartup(startup)) return false;
    return InstallImpl(created, generation, creationNext, &startup) == NativeRoutingStatus::installed;
}

NativeRoutingStatus NativeRoutingInstaller::InstallImpl(const CreatedDeviceBinding& created,
    std::uint64_t generation, void* creationNext, const NativeSettingsStartup* startup) noexcept
{
    if (g_contextHooks.load(std::memory_order_acquire) != nullptr)
        return NativeRoutingStatus::residentRecordUnavailable;
    if (!ValidCreation(created, generation))
        return NativeRoutingStatus::unqualified;
    NativeBindingPreparation preparation;
    preparation.startup = startup;
    if (!preparation.Prepare(created, creationNext)) return NativeRoutingStatus::unqualified;
    if (residentClaim.test_and_set(std::memory_order_acq_rel)) return NativeRoutingStatus::residentRecordUnavailable;
    auto* context = preparation.Publish(created, generation);
    if (context == nullptr) return NativeRoutingStatus::preparationFailed;
    if (InstallNativeSlots(*context) && CompleteCoverage())
        return NativeRoutingStatus::installed;
    WithdrawFailedInstallation(*context, startup != nullptr);
    return NativeRoutingStatus::installationFailed;
}

namespace {

class SettingsResourcePreparation
{
public:
    explicit SettingsResourcePreparation(ContextHookRecord& record) noexcept : record_(record)
    { record_.internalWork = true; }
    ~SettingsResourcePreparation() noexcept { record_.internalWork = false; }
    SettingsResourcePreparation(const SettingsResourcePreparation&) = delete;
    SettingsResourcePreparation& operator=(const SettingsResourcePreparation&) = delete;
private:
    ContextHookRecord& record_;
};

bool ActivateNativeSettings(ContextHookRecord& record, const NativeSettingsStartup& startup) noexcept
{
    try
    {
        ComPtr<ID3D11DeviceContext1> context;
        if (FAILED(record.owned->QueryInterface(IID_PPV_ARGS(&context)))) return false;
        const auto& chain = residentChain.description;
        const NativeSettingsHostConfiguration configuration{residentDevice.owned, context.Get(),
            &record.presentation, PresentationScope::qualifiedExclusive, chain.OutputWindow,
            chain.BufferDesc.Width, chain.BufferDesc.Height, record.generation,
            startup.configuration.persistencePath, startup.configuration.settings};
        const SettingsResourcePreparation preparation{record};
        return record.settingsHost.InitializeCold(configuration, startup.prepared);
    }
    catch (...) { return false; }
}

bool FreshSettingsOwner(const CreatedDeviceBinding& created, const ContextHookRecord* record) noexcept
{
    return record != nullptr && MatchesFreshObjects(created, *record) &&
        FreshOwnerSafe(*record) && NativeRoutingOwned();
}

}  // namespace

bool NativeRoutingInstaller::ActivateSettingsHost(const CreatedDeviceBinding& created,
    const NativeSettingsStartup& startup) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (!FreshSettingsOwner(created, record)) return false;
    const NativeRoutingScope scope{created.context};
    auto* admitted = scope.Record();
    if (admitted == nullptr || !admitted->queries.CanDispatch()) return false;
    if (!admitted->hdrPrepared || !QualifiedSettingsStartup(startup)) return false;
    return ActivateNativeSettings(*admitted, startup);
}

bool NativeRoutingInstaller::DiscardFresh(const CreatedDeviceBinding& created) noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return true;
    if (!MatchesFreshObjects(created, *record)) return true;
    if (!FreshOwnerSafe(*record)) { RevokeNativeRouting(); return false; }
    // Refused UI quiescence retains every published borrower and forbids retry.
    if (!record->settingsHost.RetireCold()) return false;
    // Only the creation callback can discard this not-yet-escaped candidate.
    RevokeNativeRouting();
    if (!RestoreFreshTables(*record)) return false;
    if (!RetireNativeOwners(*record)) return false;
    record->resourcesRetired = true;
    ReleaseFreshReferences(*record);
    // Immutable raw identity values/next pointers remain comparison-only after release.
    return true;
}

bool NativeRoutingInstaller::Retire() noexcept
{
    auto* record = g_contextHooks.load(std::memory_order_acquire);
    if (record == nullptr) return true;
    if (!record->retainedReferences.load(std::memory_order_acquire)) return true;
    record->presentationProxy.RevokePublication();
    if (GetCurrentThreadId() != record->thread) { RevokeNativeRouting(); return false; }
    if (record->resourcesRetired) return true;
    // Query safety must be checked before revocation resets observation coverage.
    if (!NativeRetirementSafe(*record)) { RevokeNativeRouting(); return false; }
    if (!record->settingsHost.RetireCold()) return false;
    RevokeNativeRouting();
    record->resourcesRetired = RetireNativeOwners(*record);
    return record->resourcesRetired;
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(ID3D11Device* device, const void* bytecode,
    SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept
{
    const auto* record = g_deviceHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    const NativeRoutingScope scope{device};
    const auto result = NativeResult(record->nextCreatePixelShader, device, bytecode,
        bytecodeLength, linkage, shader);
    if (auto* admitted = scope.Record(); admitted != nullptr)
    {
        if (result == S_OK && shader != nullptr)
        {
            static_cast<void>(admitted->renderer.ObserveCreatedPixelShader(
                {static_cast<const std::byte*>(bytecode), bytecodeLength}, *shader));
            static_cast<void>(admitted->uiCapture.ObserveCreatedPixelShader(
                {static_cast<const std::byte*>(bytecode), bytecodeLength}, *shader));
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* description, const D3D11_SUBRESOURCE_DATA* initialData,
    ID3D11Texture2D** texture) noexcept
{
    const auto* record = g_deviceHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    const bool ownedRequest = device == record->owned && NativeTextureRequest(description, initialData, texture);
    if (!ownedRequest)
        return NativeResult(record->nextCreateTexture2D, device, description, initialData, texture);
    const NativeRoutingScope scope{device};
    auto* context = scope.Record();
    if (PromotableNativeRequest(context, *description))
        return CreatePromotedScene(*record, *context, *description, texture);
    return NativeResult(record->nextCreateTexture2D, device, description, initialData, texture);
}

HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description, ID3D11ShaderResourceView** view) noexcept
{
    const auto* record = g_deviceHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    if (CompatibleSrvRequest(device, resource, description))
    {
        auto compatible = *description;
        compatible.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return NativeResult(record->nextCreateSrv, device, resource, &compatible, view);
    }
    return NativeResult(record->nextCreateSrv, device, resource, description, view);
}

HRESULT STDMETHODCALLTYPE HookCreateUav(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* description, ID3D11UnorderedAccessView** view) noexcept
{
    const auto* record = g_deviceHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    return NativeResult(record->nextCreateUav, device, resource, description, view);
}

HRESULT STDMETHODCALLTYPE HookCreateRtv(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* description, ID3D11RenderTargetView** view) noexcept
{
    const auto* record = g_deviceHooks.load(std::memory_order_acquire);
    if (record == nullptr) return E_UNEXPECTED;
    if (CompatibleRtvRequest(device, resource, description))
    {
        auto compatible = *description;
        compatible.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return NativeResult(record->nextCreateRtv, device, resource, &compatible, view);
    }
    return NativeResult(record->nextCreateRtv, device, resource, description, view);
}

static_assert(std::is_convertible_v<decltype(&HookCreatePixelShader), CreatePixelShader>);
static_assert(std::is_convertible_v<decltype(&HookCreateTexture2D), CreateTexture2D>);
static_assert(std::is_convertible_v<decltype(&HookCreateSrv), CreateShaderResourceView>);
static_assert(std::is_convertible_v<decltype(&HookCreateUav), CreateUnorderedAccessView>);
static_assert(std::is_convertible_v<decltype(&HookCreateRtv), CreateRenderTargetView>);

}  // namespace fo4hdr
