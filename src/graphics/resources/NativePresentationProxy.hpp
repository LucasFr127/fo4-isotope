// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace fo4hdr {

class ScenePassRenderer;
class PresentationPass;
class NativePresentationProxy;
struct SceneTextureViews;

enum class NativeSdrSourceRole
{
    logicalBuffer,
    ownedProxy
};

enum class NativeSdrCaptureScope
{
    unavailable,
    completedAuthoredCodesExclusive
};

// Borrowed immutable code snapshot; owner and scene renderer outlive its consumer.
class CompletedNativeSdrFrame
{
public:
    [[nodiscard]] bool ReadyFor(std::uint64_t frame, const ScenePassRenderer* scenes) const noexcept;

private:
    friend class NativePresentationProxy;
    friend class PresentationPass;
    const NativePresentationProxy* owner_{};
    const ScenePassRenderer* scenes_{};
    ID3D11ShaderResourceView* source_{};
    std::uint64_t epoch_{};
    std::uint64_t frame_{};
};

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
    // Borrowed owned storage until Retire; registration establishes no pixel domain.
    [[nodiscard]] SceneTextureViews SceneViews() const noexcept;
    // Caller proves completed authored SDR-code production, immutable selected
    // source and the exclusive query/list/reentry scope; storage does not prove codes.
    // Copies once into private same-format storage, with no wait or retained source alias.
    [[nodiscard]] bool CaptureCompletedSdr(NativeSdrSourceRole role,
        ScenePassRenderer& scenes, NativeSdrCaptureScope scope) noexcept;
    [[nodiscard]] CompletedNativeSdrFrame CompletedFrame() const noexcept;
    // Owner-thread close clears frame publication without touching context bindings.
    void InvalidateFrame() noexcept;
    // Foreign-thread/reentry revocation touches only atomic availability.
    void RevokePublication() noexcept;
    // Unbind only this proxy's aliases, preserving all foreign bindings and counters.
    [[nodiscard]] bool Retire() noexcept;

private:
    friend class CompletedNativeSdrFrame;
    struct CodeSnapshot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
    };
    struct Resources
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        Microsoft::WRL::ComPtr<IUnknown> backBufferIdentity;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> proxy;
        Microsoft::WRL::ComPtr<IUnknown> proxyIdentity;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
        std::array<CodeSnapshot, 2> snapshots;
        DXGI_FORMAT nativeFormat{DXGI_FORMAT_UNKNOWN};
    };

    [[nodiscard]] static bool PrepareResources(ID3D11Device* device,
        ID3D11DeviceContext* context, IDXGISwapChain* chain, Resources& candidate) noexcept;
    [[nodiscard]] static bool CaptureBackBuffer(ID3D11Device* device,
        IDXGISwapChain* chain, Resources& candidate) noexcept;
    [[nodiscard]] static bool PrepareProxy(ID3D11Device* device, Resources& candidate) noexcept;
    [[nodiscard]] static bool PrepareSnapshot(ID3D11Device* device,
        ID3D11Texture2D* source, CodeSnapshot& snapshot) noexcept;
    [[nodiscard]] static bool PrepareSnapshots(ID3D11Device* device, Resources& candidate) noexcept;
    [[nodiscard]] bool CanCapture(const ScenePassRenderer& scenes, NativeSdrCaptureScope scope) const noexcept;
    [[nodiscard]] bool SubmitCapture(NativeSdrSourceRole role, ScenePassRenderer& scenes) noexcept;
    [[nodiscard]] bool CanPublishCapture(const ScenePassRenderer& scenes, std::uint64_t frame) const noexcept;
    [[nodiscard]] bool FrameReady(std::uint64_t frame, const ScenePassRenderer* scenes) const noexcept;
    [[nodiscard]] bool PacketIdentityCurrent(const CompletedNativeSdrFrame& packet) const noexcept;
    [[nodiscard]] bool MatchesSceneOwner(const ScenePassRenderer& scenes) const noexcept;
    [[nodiscard]] bool PacketReady(const CompletedNativeSdrFrame& packet,
        std::uint64_t frame, const ScenePassRenderer* scenes) const noexcept;
    [[nodiscard]] bool CanResolve() const noexcept;
    [[nodiscard]] bool OnOwnerThread() const noexcept;

    Resources resources_;
    std::atomic<bool> publicationAllowed_{};
    const ScenePassRenderer* scenes_{};
    std::uint64_t epoch_{};
    std::uint64_t frame_{};
    std::uint64_t lastCapturedFrame_{};
    std::size_t snapshotIndex_{};
    std::thread::id ownerThread_;
    mutable bool executing_{};
};

}  // namespace fo4hdr
