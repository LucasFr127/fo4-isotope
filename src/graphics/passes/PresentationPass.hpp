// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../HdrRenderer.hpp"
#include "../resources/GpuPerformanceQueries.hpp"

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace fo4hdr {

class SettingsWindow;
struct SettingsWindowOverlay;
class CompletedNativeUiLayers;

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
    // Borrowed settings owner uses this same context/thread and outlives this consumer.
    SettingsWindow* settingsWindow{};
    bool measurePerformance{};
};

// Serialized initialization-thread calls on a genuine exclusively owned context.
// Retire this consumer before the borrowed scene owner or context qualification;
// the borrowed scene owner outlives this owner. qualifiedExclusive asserts no
// foreign active queries, command lists or concurrent/reentrant context mutation;
// only this owner may bracket its final dispatch with diagnostic queries;
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
    // Closed-frame numeric changes prepare both owners before publishing either tuple.
    [[nodiscard]] bool ApplySettings(const OperatorLutData& table,
        LayerWhiteConfiguration whites, PresentationScope scope,
        SceneGradeConfiguration sceneGrade = {}) noexcept;
    // Supply a fresh caller-owned SDR-baseline candidate before any SetColorSpace1.
    // Numeric LUT/peak/white changes retain the chain. Device, descriptor or display
    // changes revoke qualification; retire before resize.
    HRESULT PrepareChain(IDXGISwapChain* chain) noexcept;
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] GpuPerformanceSnapshot Performance() const noexcept;
    void PollPerformance(PresentationScope scope) noexcept;
    // Read-only qualification for closed-frame commits that need no native change.
    [[nodiscard]] bool CanApplySettings(PresentationScope scope) const noexcept;
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
    // Only a completed capture packet admits FP16 authored backdrop codes.
    // Its owner/frame qualification remains live through this guarded preparation.
    [[nodiscard]] bool PrepareNativeLayers(const CompletedNativeUiLayers& layers,
        PresentationScope scope) noexcept;
    // Caller qualifies a completed authored SDR frame, independently of texture format.
    // Copies matching R8_UNORM codes into private FP16 storage for this open frame;
    // no caller view is retained. Intercepted pixels need separate semantic proof.
    [[nodiscard]] bool PrepareSdrHandoff(ID3D11ShaderResourceView* completedAuthoredCodes,
        PresentationScope scope) noexcept;
    // Nonfatal TEST leaves the frame open; device removal/reset revokes publication.
    // Without a successful BeginFrame, qualified
    // presentation clears black but does not compose; unavailable scope cannot clear.
    HRESULT Present(IDXGISwapChain* chain, UINT interval, UINT flags,
        PresentationScope scope) noexcept;
    // Borrowed state is restored before the caller invokes its captured native next.
    void PrepareNativePresent(IDXGISwapChain* chain, UINT flags, PresentationScope scope) noexcept;
    void CompleteNativePresent(IDXGISwapChain* chain, HRESULT nativeResult) noexcept;

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

    struct CodedLayer
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
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
    [[nodiscard]] bool QualifiedSettingsSource(ID3D11ShaderResourceView* source,
        ID3D11UnorderedAccessView* terminal) const noexcept;
    [[nodiscard]] bool ReadSettingsOverlay(std::uint64_t frame,
        ID3D11UnorderedAccessView* terminal, SettingsWindowOverlay& overlay) noexcept;
    void ComposeChain(PresentationScope scope) noexcept;
    [[nodiscard]] bool ComposeHdrChain(ID3D11UnorderedAccessView* target,
        PresentationScope scope) noexcept;
    [[nodiscard]] bool ComposeSdrChain(ID3D11UnorderedAccessView* target) noexcept;
    [[nodiscard]] bool SetHdrColorSpace(bool hdr) noexcept;
    [[nodiscard]] bool CopySdrCodes(ID3D11ShaderResourceView* source,
        ID3D11UnorderedAccessView* target) noexcept;
    [[nodiscard]] bool QualifiedLayerSources(ID3D11ShaderResourceView* backdrop,
        ID3D11ShaderResourceView* hud, DXGI_FORMAT backdropFormat) const noexcept;
    [[nodiscard]] bool QualifiedLayerSource(ID3D11ShaderResourceView* source,
        Microsoft::WRL::ComPtr<IUnknown>& identity, DXGI_FORMAT format) const noexcept;
    [[nodiscard]] bool PrepareLayerSources(ID3D11ShaderResourceView* backdrop,
        ID3D11ShaderResourceView* hud, PresentationScope scope, DXGI_FORMAT backdropFormat) noexcept;
    [[nodiscard]] bool CopyLayerSources(ID3D11ShaderResourceView* backdrop,
        ID3D11ShaderResourceView* hud, DXGI_FORMAT backdropFormat) noexcept;
    [[nodiscard]] static bool PrepareLayer(ID3D11Device* device,
        const PresentationConfiguration& configuration, LinearLayer& prepared) noexcept;
    [[nodiscard]] static bool PrepareCodedLayer(ID3D11Device* device,
        const PresentationConfiguration& configuration, DXGI_FORMAT format, CodedLayer& prepared) noexcept;
    void ClearLayers() noexcept;
    [[nodiscard]] bool ComposeSelected(SceneTextureViews source,
        ID3D11UnorderedAccessView* terminal) noexcept;

    RendererLifetime lifetime_;
    std::thread::id ownerThread_;
    PresentationConfiguration configuration_;
    GpuPerformanceQueries performance_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 4> constants_;
    std::array<CodedLayer, 3> layers_;
    std::array<ID3D11ShaderResourceView*, 2> selectedLayers_{};
    LinearLayer sdrProxy_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> sdrShader_;
    std::uint64_t sdrFrame_{};
    bool hdrColorSpace_{};
    std::uint64_t layerFrame_{};
    std::uint64_t layerFailureFrame_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain_;
    ChainBuffer buffer_;
    bool framePrepared_{};
    bool executing_{};
};

}  // namespace fo4hdr
