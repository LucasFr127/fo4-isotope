// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DeveloperExposureLock.hpp"
#include "RendererLifetime.hpp"
#include "resources/DeveloperExposureResources.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace fo4hdr {

class ScenePassRenderer;
class PresentationPass;
struct OperatorLutData;

struct LayerWhiteConfiguration
{
    float uiWhiteNits{static_cast<float>(kSceneReferenceWhiteNits)};
    float videoWhiteNits{static_cast<float>(kSceneReferenceWhiteNits)};

    [[nodiscard]] bool Valid() const noexcept;
};

struct ScenePassConfiguration
{
    UINT width{};
    UINT height{};
    LayerWhiteConfiguration whites;
    // Borrowed only during Initialize; a requested invalid table fails initialization.
    const OperatorLutData* operatorLut{};
};

class SceneTextureHandle
{
public:
    SceneTextureHandle() noexcept = default;

private:
    friend class ScenePassRenderer;
    const ScenePassRenderer* owner_{};
    std::uint64_t epoch_{};
    std::size_t slot_{};
};

struct SceneTextureViews
{
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* shaderResource{};
    ID3D11RenderTargetView* renderTarget{};
};

struct FiniteDrawInvocation
{
    // The caller issues exactly one finite draw and does not mutate other state.
    void (*draw)(ID3D11DeviceContext*, void*){};
    void* argument{};
};

enum class SceneDrawOutcome : std::uint8_t
{
    invalidInvocation,
    forwarded,
    replaced,
    callbackFailed
};

enum class SceneColorDomain : std::uint8_t
{
    // Linear BT.709/D65; one scene unit represents 203 nits at presentation.
    bt709Scene = 0,
    // Linear BT.2020/D65 divided by the shared cold UI white, including video.
    bt2020UiRelative = 1
};

// One exclusively owned immediate context, one initialization thread and one
// nonrecycled generation. Handles/views expire at Retire or owner destruction;
// do not retain them across destruction and reconstruction at the same address.
class ScenePassRenderer
{
public:
    static constexpr std::size_t sceneTextureCapacity = 8;
    static constexpr std::size_t observedShaderCapacity = 64;
    static constexpr std::size_t replacementProgramCount = 9;

    ScenePassRenderer() noexcept = default;
    ~ScenePassRenderer() noexcept;
    ScenePassRenderer(const ScenePassRenderer&) = delete;
    ScenePassRenderer& operator=(const ScenePassRenderer&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, UINT width, UINT height) noexcept;
    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, ScenePassConfiguration configuration) noexcept;
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] bool MatchesContext(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext) const noexcept;
    [[nodiscard]] bool MatchesLayerWhites(LayerWhiteConfiguration whites) const noexcept;

    HRESULT CreateObservedPixelShader(std::span<const std::byte> bytecode,
        ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept;
    HRESULT CreateSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        const D3D11_SUBRESOURCE_DATA* initialData, SceneTextureHandle& handle) noexcept;
    [[nodiscard]] SceneTextureViews Views(SceneTextureHandle handle) const noexcept;

    [[nodiscard]] bool BeginFrame() noexcept;
    [[nodiscard]] bool EndFrame() noexcept;
    // Controlled diagnostic only: EndFrame alone never qualifies activation.
    // Save FrameGeneration before close; report eligibility once after that close.
    // The caller excludes active queries and command-list execution throughout
    // capture/replay; idle state getters cannot establish their absence.
    [[nodiscard]] bool CompleteExposureFrame(std::uint64_t savedFrame, bool eligible) noexcept;
    [[nodiscard]] bool SetExposureLockRequested(bool requested) noexcept;
    [[nodiscard]] bool ExposureLockActive() const noexcept;
    // Query at consumption: retained handles remain storage-valid after lineage expires.
    [[nodiscard]] SceneTextureHandle CurrentScene() const noexcept;
    [[nodiscard]] SceneColorDomain CurrentSceneDomain() const noexcept;
    // Zero denotes a closed or unavailable frame; completed layers use this token.
    [[nodiscard]] std::uint64_t FrameGeneration() const noexcept;
    // Report every external write through a borrowed alias before it occurs.
    [[nodiscard]] bool InvalidateScene(SceneTextureHandle handle) noexcept;

    // Direct execution consumes the caller's independently qualified descriptor;
    // it does not establish an original-bytecode match.
    // FXAA requires the positive-HDR source contract; video requires completed
    // authored BGR codes, and publishes a separate UI-relative BT.2020 domain.
    // Video b0 and grade/temporal b13 nonnull ranges are independently caller-qualified;
    // preserve the actual reported tuple without inferring legacy whole-buffer windows.
    // Temporal metadata/history require R16G16B16A16_FLOAT or R32G32B32A32_FLOAT
    // views; the caller separately qualifies their native float4 semantics.
    // Operator/peak changes require independently reset or requalified temporal history.
    [[nodiscard]] SceneDrawOutcome Execute(const RenoDxShaderDescriptor& descriptor,
        SceneTextureHandle input, SceneTextureHandle destination,
        FiniteDrawInvocation invocation) noexcept;
    [[nodiscard]] SceneDrawOutcome ExecuteObserved(SceneTextureHandle input,
        SceneTextureHandle destination, FiniteDrawInvocation invocation) noexcept;

private:
    friend class PresentationPass;
    [[nodiscard]] bool ApplySettings(const OperatorLutData& table,
        LayerWhiteConfiguration whites) noexcept;
    void MarkSettingsStale() noexcept;
    struct SceneTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResource;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
        bool toneProduced{};
        bool operatorProduced{};
        bool settingsStale{};
        SceneColorDomain domain{SceneColorDomain::bt709Scene};
    };

    struct ObservedShader
    {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> original;
        const RenoDxShaderDescriptor* descriptor{};
    };

    struct DrawLineage
    {
        bool consumesSelected{};
        bool toneInput{};
        bool selectionExisted{};
        std::uint8_t writes{};
        SceneColorDomain domain{SceneColorDomain::bt709Scene};
        bool operatorProduced{};
    };

    struct DrawReplacement
    {
        ID3D11PixelShader* shader{};
        ID3D11Buffer* videoConstants{};
        ID3D11Buffer* operatorConstants{};
        bool tone{};
        bool capture{};
        bool replay{};
    };

    struct DrawResult
    {
        SceneDrawOutcome outcome{SceneDrawOutcome::forwarded};
        bool captured{};
        bool replayed{};
        bool exposureFailed{};
    };

    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool CanInvoke(FiniteDrawInvocation invocation) const noexcept;
    [[nodiscard]] const SceneTexture* Resolve(SceneTextureHandle handle) const noexcept;
    [[nodiscard]] const RenoDxShaderDescriptor* BoundDescriptor() const noexcept;
    [[nodiscard]] bool PromotableScene(const D3D11_TEXTURE2D_DESC& original) const noexcept;
    HRESULT PrepareSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        SceneTexture& prepared) noexcept;
    [[nodiscard]] bool BoundSceneViewsMatch(const RenoDxShaderDescriptor& descriptor,
        const SceneTexture& source, const SceneTexture& target) const noexcept;
    [[nodiscard]] bool BoundTemporalViewsMatch(const RenoDxShaderDescriptor& descriptor,
        const SceneTexture& source, const SceneTexture& target) const noexcept;
    [[nodiscard]] std::uint8_t BoundWriteMask() const noexcept;
    void ClearLineage() noexcept;
    void InvalidateSlot(std::size_t slot) noexcept;
    void InvalidateWrites(std::uint8_t writes) noexcept;
    [[nodiscard]] std::uint8_t WriteMaskForResource(ID3D11Resource* resource) const noexcept;
    [[nodiscard]] DrawLineage CaptureLineage(SceneTextureHandle input) const noexcept;
    void PublishLineage(const RenoDxShaderDescriptor& descriptor,
        SceneTextureHandle destination, const DrawLineage& lineage) noexcept;
    void PublishOutputDomain(RenoDxShaderPass pass,
        SceneTextureHandle destination, const DrawLineage& lineage) noexcept;
    void ResetOperatorResources() noexcept;
    void CompleteLineage(const RenoDxShaderDescriptor* descriptor,
        SceneTextureHandle destination, SceneDrawOutcome outcome,
        const DrawLineage& lineage) noexcept;
    [[nodiscard]] DrawReplacement PrepareDrawReplacement(
        const RenoDxShaderDescriptor* descriptor, SceneTextureHandle input,
        SceneTextureHandle destination) const noexcept;
    [[nodiscard]] DrawResult InvokeDraw(DrawReplacement replacement,
        FiniteDrawInvocation invocation) noexcept;
    void PrepareExposureReplacement(DrawReplacement& replacement,
        RenoDxShaderPass pass) const noexcept;
    [[nodiscard]] DrawResult PrepareExposureDraw(const DrawReplacement& replacement) noexcept;
    void CompleteExposureDraw(const DrawResult& result, bool tone) noexcept;
    void ResetExposurePolicy() noexcept;
    [[nodiscard]] ID3D11Buffer* ConstantsForInput(SceneTextureHandle input) const noexcept;
    [[nodiscard]] ID3D11PixelShader* QualifiedReplacement(
        const RenoDxShaderDescriptor* descriptor, SceneTextureHandle input,
        SceneTextureHandle destination) const noexcept;
    [[nodiscard]] SceneDrawOutcome Invoke(const RenoDxShaderDescriptor* descriptor,
        SceneTextureHandle input, SceneTextureHandle destination,
        FiniteDrawInvocation invocation) noexcept;

    RendererLifetime lifetime_;
    std::thread::id ownerThread_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> videoConstants_;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> operatorTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> operatorSource_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> operatorSampler_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> operatorConstants_;
    bool operatorEnabled_{};
    LayerWhiteConfiguration whites_;
    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, replacementProgramCount> replacements_;
    std::array<SceneTexture, sceneTextureCapacity> scenes_;
    std::array<ObservedShader, observedShaderCapacity> observed_;
    std::size_t sceneCount_{};
    std::size_t observedCount_{};
    UINT width_{};
    UINT height_{};
    bool invoking_{};
    SceneTextureHandle selectedScene_;
    bool frameOpen_{};
    std::uint64_t frameGeneration_{};
    DeveloperExposureLock exposureLock_;
    DeveloperExposureResources exposureResources_;
    std::uint64_t exposureGeneration_{};
    std::uint64_t endedExposureFrame_{};
};

}  // namespace fo4hdr
