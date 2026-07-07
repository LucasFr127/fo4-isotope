// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../HdrRenderer.hpp"

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <thread>

namespace fo4hdr {

enum class PresentationScope
{
    unavailable,
    qualifiedExclusive
};

struct PresentationConfiguration
{
    UINT width{};
    UINT height{};
    float displayPeakNits{};
    ScenePassRenderer* scenes{};
};

// Serialized initialization-thread calls on a genuine exclusively owned context.
// Retire this consumer before the borrowed scene owner or context qualification;
// the borrowed scene owner outlives this owner. qualifiedExclusive asserts no
// active queries, command lists or concurrent/reentrant context mutation;
// it is a caller qualification, not an automatically discovered property.
class PresentationPass
{
public:
    PresentationPass() noexcept = default;
    ~PresentationPass() noexcept;
    PresentationPass(const PresentationPass&) = delete;
    PresentationPass& operator=(const PresentationPass&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, PresentationConfiguration configuration) noexcept;
    // Supply a fresh caller-owned SDR-baseline candidate before any SetColorSpace1.
    // Display/configuration changes revoke this qualification; retire before resize.
    HRESULT PrepareChain(IDXGISwapChain* chain) noexcept;
    [[nodiscard]] bool Active() const noexcept;
    // Cold retirement/destruction requires the exclusive context scope above.
    [[nodiscard]] bool Retire() noexcept;

    // No external Present/Resize or target write between BeginFrame and Present.
    [[nodiscard]] bool BeginFrame(PresentationScope scope) noexcept;
    [[nodiscard]] bool ExecuteCurrentScene(ID3D11UnorderedAccessView* terminal,
        PresentationScope scope) noexcept;
    // Nonfatal TEST leaves the frame open; device removal/reset revokes publication.
    // Without a successful BeginFrame, qualified
    // presentation clears black but does not compose; unavailable scope cannot clear.
    HRESULT Present(IDXGISwapChain* chain, UINT interval, UINT flags,
        PresentationScope scope) noexcept;

private:
    struct ChainBuffer
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> target;
    };

    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool CanExecute(PresentationScope scope) const noexcept;
    [[nodiscard]] bool CanPrepareChain() const noexcept;
    [[nodiscard]] bool CanRetire() const noexcept;
    HRESULT PrepareBuffer(IDXGISwapChain3* chain, ChainBuffer& prepared) const noexcept;
    [[nodiscard]] bool UnbindChainBuffer() noexcept;
    void CloseFrame() noexcept;
    [[nodiscard]] bool SameChain(IDXGISwapChain* chain) const noexcept;
    [[nodiscard]] ID3D11UnorderedAccessView* CurrentTarget() const noexcept;
    [[nodiscard]] bool QualifiedTarget(ID3D11UnorderedAccessView* terminal,
        ID3D11Texture2D* scene) const noexcept;
    void ComposeChain(PresentationScope scope) noexcept;

    RendererLifetime lifetime_;
    std::thread::id ownerThread_;
    PresentationConfiguration configuration_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain_;
    ChainBuffer buffer_;
    bool framePrepared_{};
    bool executing_{};
};

}  // namespace fo4hdr
