// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <thread>

namespace fo4hdr {

// Serialized owner-thread calls on a genuine same-device immediate context.
// The caller excludes queries, command lists and concurrent/reentrant mutation.
// Retire before ResizeBuffers and before destroying the borrowed context owner.
class NativePresentationProxy final
{
public:
    NativePresentationProxy() noexcept = default;
    ~NativePresentationProxy() noexcept;
    NativePresentationProxy(const NativePresentationProxy&) = delete;
    NativePresentationProxy& operator=(const NativePresentationProxy&) = delete;

    // Cold transaction; native buffer storage and view results remain unchanged.
    [[nodiscard]] bool Prepare(ID3D11Device* device, ID3D11DeviceContext* context,
        IDXGISwapChain* chain) noexcept;
    // A borrowed target valid only until this owner's retirement. Matching a
    // native view establishes identity, not permission to redirect or a color domain.
    [[nodiscard]] ID3D11RenderTargetView* ResolveRenderTarget(
        ID3D11RenderTargetView* nativeTarget) const noexcept;
    // Unbind only this proxy's aliases, preserving all foreign bindings and counters.
    [[nodiscard]] bool Retire() noexcept;

private:
    struct Resources
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        Microsoft::WRL::ComPtr<IUnknown> backBufferIdentity;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> proxy;
        Microsoft::WRL::ComPtr<IUnknown> proxyIdentity;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
        DXGI_FORMAT nativeFormat{DXGI_FORMAT_UNKNOWN};
    };

    [[nodiscard]] static bool PrepareResources(ID3D11Device* device,
        ID3D11DeviceContext* context, IDXGISwapChain* chain, Resources& candidate) noexcept;
    [[nodiscard]] static bool CaptureBackBuffer(ID3D11Device* device,
        IDXGISwapChain* chain, Resources& candidate) noexcept;
    [[nodiscard]] static bool PrepareProxy(ID3D11Device* device, Resources& candidate) noexcept;
    [[nodiscard]] bool CanResolve() const noexcept;
    [[nodiscard]] bool OnOwnerThread() const noexcept;

    Resources resources_;
    std::thread::id ownerThread_;
    mutable bool executing_{};
};

}  // namespace fo4hdr
