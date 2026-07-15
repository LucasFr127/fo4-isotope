// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../HdrRenderer.hpp"

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
    LayerWhiteConfiguration whites;
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
    // Caller qualifies completed authored codes and coverage, not intercepted UI.
    // Backdrop RGB is its completed contribution; HUD nonnegative SDR code RGB
    // is premultiplied by coverage before storage, not linear-light premultiplication.
    // Null views denote absent layers; borrowed sources remain immutable in this call.
    // R8G8B8A8_UNORM (not SRGB) preserves code-domain alpha unpremultiplication.
    // Failed required preparation blocks composition until successful preparation
    // or the next producer frame, so failure cannot silently discard required UI.
    [[nodiscard]] bool PrepareLayers(ID3D11ShaderResourceView* backdrop,
        ID3D11ShaderResourceView* hud, PresentationScope scope) noexcept;
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

    // Views never escape this owner or remain bound beyond a guarded dispatch.
    struct LinearLayer
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
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
    [[nodiscard]] bool QualifiedLayerSources(ID3D11ShaderResourceView* backdrop,
        ID3D11ShaderResourceView* hud) const noexcept;
    [[nodiscard]] bool QualifiedLayerSource(ID3D11ShaderResourceView* source,
        Microsoft::WRL::ComPtr<IUnknown>& identity) const noexcept;
    [[nodiscard]] static bool PrepareLayer(ID3D11Device* device,
        const PresentationConfiguration& configuration, LinearLayer& prepared) noexcept;
    void ClearLayers() noexcept;
    [[nodiscard]] bool ComposeSelected(SceneTextureViews source,
        ID3D11UnorderedAccessView* terminal) noexcept;

    RendererLifetime lifetime_;
    std::thread::id ownerThread_;
    PresentationConfiguration configuration_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> constants_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ComputeShader>, 2> layerShaders_;
    std::array<LinearLayer, 2> layers_;
    std::uint64_t layerFrame_{};
    std::uint64_t layerFailureFrame_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain_;
    ChainBuffer buffer_;
    bool framePrepared_{};
    bool executing_{};
};

}  // namespace fo4hdr
