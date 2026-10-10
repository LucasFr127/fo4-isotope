// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "../DrawObservation.hpp"
#include "FO4Isotope/hooks/HookOwnership.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

inline constexpr std::size_t kMaximumVtableSlots = 512;

inline constexpr std::size_t kDeviceCreateTexture2D = 5;
inline constexpr std::size_t kRelease = 2;
inline constexpr std::size_t kDeviceCreateSrv = 7;
inline constexpr std::size_t kDeviceCreateUav = 8;
inline constexpr std::size_t kDeviceCreateRtv = 9;
inline constexpr std::size_t kDeviceCreatePixelShader = 15;
inline constexpr std::size_t kDeviceBaseSlots = 43;

inline constexpr std::size_t kContextPsSetShader = 9;
inline constexpr std::size_t kContextDrawIndexed = 12;
inline constexpr std::size_t kContextDraw = 13;
inline constexpr std::size_t kContextDrawIndexedInstanced = 20;
inline constexpr std::size_t kContextDrawInstanced = 21;
// SDK 10.0.26100.0 ID3D11DeviceContext vtable, including IUnknown/DeviceChild.
inline constexpr std::size_t kContextBegin = 27;
inline constexpr std::size_t kContextEnd = 28;
inline constexpr std::size_t kContextOmSetRenderTargets = 33;
inline constexpr std::size_t kContextOmSetRenderTargetsAndUavs = 34;
inline constexpr std::size_t kContextDispatch = 41;
inline constexpr std::size_t kContextDispatchIndirect = 42;
inline constexpr std::size_t kContextCopySubresourceRegion = 46;
inline constexpr std::size_t kContextCopyResource = 47;
inline constexpr std::size_t kContextClearRenderTargetView = 50;
inline constexpr std::size_t kContextClearUnorderedAccessViewUint = 51;
inline constexpr std::size_t kContextClearUnorderedAccessViewFloat = 52;
inline constexpr std::size_t kContextExecuteCommandList = 58;
inline constexpr std::size_t kContextBaseSlots = 115;

inline constexpr std::size_t kSwapChainPresent = 8;
inline constexpr std::size_t kSwapChainResizeBuffers = 13;
inline constexpr std::size_t kSwapChainBaseSlots = 18;

using CreateTexture2D = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateShaderResourceView = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, ID3D11Resource*,
    const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using CreateUnorderedAccessView = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, ID3D11Resource*,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**);
using CreateRenderTargetView = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, ID3D11Resource*,
    const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
using CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
    ID3D11PixelShader**);
using PsSetShader = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
    UINT);
using DrawIndexed = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, INT);
using Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstanced = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using BeginQuery = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
using EndQuery = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
using ExecuteCommandList = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using CopySubresourceRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ClearRenderTargetView = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
using Dispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using ClearUnorderedAccessViewUint = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT*);
using ClearUnorderedAccessViewFloat = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT*);
using OmSetRenderTargets = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*);
using OmSetRenderTargetsAndUavs = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*, UINT, UINT,
    ID3D11UnorderedAccessView* const*, const UINT*);
using ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using DeviceRelease = ULONG(STDMETHODCALLTYPE*)(ID3D11Device*);
using ContextRelease = ULONG(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using SwapChainRelease = ULONG(STDMETHODCALLTYPE*)(IDXGISwapChain*);
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

struct DeviceRecord
{
    ObjectGenerationIdentity identity{};
    void** originalVtable{};
    DeviceRelease nextRelease{};
    CreateTexture2D nextCreateTexture2D{};
    CreateShaderResourceView nextCreateSrv{};
    CreateUnorderedAccessView nextCreateUav{};
    CreateRenderTargetView nextCreateRtv{};
    CreatePixelShader nextCreatePixelShader{};
    std::array<void*, kMaximumVtableSlots> shadow{};
};

struct ContextRecord : DrawObservation
{
    ObjectGenerationIdentity identity{};
    void** originalVtable{};
    ContextRelease nextRelease{};
    PsSetShader nextPsSetShader{};
    DrawIndexed nextDrawIndexed{};
    Draw nextDraw{};
    DrawIndexedInstanced nextDrawIndexedInstanced{};
    DrawInstanced nextDrawInstanced{};
    BeginQuery nextBegin{};
    EndQuery nextEnd{};
    ExecuteCommandList nextExecuteCommandList{};
    CopySubresourceRegion nextCopySubresourceRegion{};
    CopyResource nextCopyResource{};
    ClearRenderTargetView nextClearRenderTargetView{};
    Dispatch nextDispatch{};
    DispatchIndirect nextDispatchIndirect{};
    ClearUnorderedAccessViewUint nextClearUnorderedAccessViewUint{};
    ClearUnorderedAccessViewFloat nextClearUnorderedAccessViewFloat{};
    OmSetRenderTargets nextOmSetRenderTargets{};
    OmSetRenderTargetsAndUavs nextOmSetRenderTargetsAndUavs{};
    std::array<void*, kMaximumVtableSlots> shadow{};
};

struct SwapChainRecord
{
    Present nextPresent{};
    bool developerHdrDescriptor{};
    bool applicationFp16Facade{};
    bool ownsRendererClaim{};
    ObjectGenerationIdentity identity{};
    void** originalVtable{};
    SwapChainRelease nextRelease{};
    ResizeBuffers nextResizeBuffers{};
    std::array<void*, kMaximumVtableSlots> shadow{};
};

inline constexpr std::size_t kMaximumHookObjects = 4;
inline constexpr std::size_t kHookRegistryAttempts = 64;
extern std::array<DeviceRecord, kMaximumHookObjects> g_devices;
extern std::array<ContextRecord, kMaximumHookObjects> g_contexts;
extern std::array<SwapChainRecord, kMaximumHookObjects> g_swapChains;
extern std::atomic_flag g_installLock;
extern std::atomic<std::uint64_t> g_objectGeneration;

DeviceRecord* FindDevice(ID3D11Device*) noexcept;
ContextRecord* FindContext(ID3D11DeviceContext*) noexcept;
SwapChainRecord* FindSwapChain(IDXGISwapChain*) noexcept;

} // namespace fo4hdr
