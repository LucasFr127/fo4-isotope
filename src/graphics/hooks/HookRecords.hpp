// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ContextHooks.hpp"
#include "../HdrRenderer.hpp"
#include "../passes/ObservedDraw.hpp"
#include "Fallout4HDR/hooks/HookOwnership.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace fo4hdr {

using CreateTexture2D = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
    const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateShaderResourceView = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
    const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using CreateUnorderedAccessView = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**);
using CreateRenderTargetView = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
    const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
using CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11PixelShader**);
using PsSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*,
    ID3D11ClassInstance* const*, UINT);
using DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OmSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OmSetRenderTargetsAndUavs = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
    ID3D11UnorderedAccessView* const*, const UINT*);
using SwapChainPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SwapChainResize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// Published once before mutation and retained for process lifetime, including
// rollback failure. Native predecessors are immutable and table-specific.
struct DeviceHookRecord
{
    ID3D11Device* owned{};
    void** table{};
    CreateTexture2D nextCreateTexture2D{};
    CreateShaderResourceView nextCreateSrv{};
    CreateUnorderedAccessView nextCreateUav{};
    CreateRenderTargetView nextCreateRtv{};
    CreatePixelShader nextCreatePixelShader{};
    std::array<PointerHookOwnership, 5> owners;
};

struct ContextHookRecord
{
    ID3D11DeviceContext* owned{};
    void** table{};
    DWORD thread{};
    std::uint64_t generation{};
    PsSetShader nextPsSetShader{};
    DrawIndexed nextDrawIndexed{};
    Draw nextDraw{};
    DrawIndexedInstanced nextDrawIndexedInstanced{};
    DrawInstanced nextDrawInstanced{};
    ContextQueryCall nextBegin{};
    ContextQueryCall nextEnd{};
    OmSetRenderTargets nextOmSetRenderTargets{};
    OmSetRenderTargetsAndUavs nextOmSetRenderTargetsAndUavs{};
    ContextCommandListCall nextExecuteCommandList{};
    std::array<PointerHookOwnership, 10> owners;
    ContextQueryObserver queries;
    ScenePassRenderer renderer;
    NativeUiCapture uiCapture;
    std::atomic<bool> admitted{};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    bool internalWork{};
};

struct SwapChainHookRecord
{
    IDXGISwapChain* owned{};
    void** table{};
    SwapChainPresent nextPresent{};
    SwapChainResize nextResize{};
    std::array<PointerHookOwnership, 2> owners;
    DXGI_SWAP_CHAIN_DESC description{};
    std::atomic<bool> admitted{};
    std::atomic<bool> resourcesReady{};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
};

extern std::atomic<DeviceHookRecord*> g_deviceHooks;
extern std::atomic<ContextHookRecord*> g_contextHooks;
extern std::atomic<SwapChainHookRecord*> g_swapChainHooks;

[[nodiscard]] bool NativeRoutingOwned() noexcept;
[[nodiscard]] bool NativeChainOwned() noexcept;
void RevokeNativeRouting() noexcept;

// Only the admitted owner thread reads internalWork or the renderer. Other
// objects sharing the table use its captured predecessor directly.
class NativeRoutingScope
{
public:
    explicit NativeRoutingScope(IUnknown* object) noexcept;
    ~NativeRoutingScope() noexcept;
    NativeRoutingScope(const NativeRoutingScope&) = delete;
    NativeRoutingScope& operator=(const NativeRoutingScope&) = delete;
    [[nodiscard]] ContextHookRecord* Record() const noexcept { return record_; }

private:
    ContextHookRecord* record_{};
};

}  // namespace fo4hdr
