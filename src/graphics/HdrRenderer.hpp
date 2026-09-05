// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DeveloperExposureLock.hpp"
#include "PipboyScreenObservation.hpp"
#include "RendererLifetime.hpp"
#include "resources/DeveloperExposureResources.hpp"
#include "resources/NativeReferenceResources.hpp"
#include "resources/NativeSceneAssociation.hpp"
#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

struct SceneGradeConfiguration
{
    // Blend native grade in scene units before the independent operator sample.
    float vanillaColorGradingStrength{1.0F};
    HdrSettingsMath imageControls{};
    ColorGradingMethod method{ColorGradingMethod::radial};

    [[nodiscard]] bool Valid() const noexcept
    {
        return (method == ColorGradingMethod::radial || method == ColorGradingMethod::nativeReference) &&
            std::isfinite(vanillaColorGradingStrength) &&
            vanillaColorGradingStrength >= 0.0F && vanillaColorGradingStrength <= 1.0F &&
            ImageScalarsValid() && MatrixValid();
    }

private:
    [[nodiscard]] bool ImageScalarsValid() const noexcept
    {
        const std::array<std::array<double, 3>, 7> ranges{{
            {imageControls.contrastSlope, 0.5, 2.0},
            {imageControls.blacksStrength, -0.75, 0.75},
            {imageControls.highlightsEv, -2.0, 2.0},
            {imageControls.shadowsEv, -2.0, 2.0},
            {imageControls.saturationScale, 0.0, 2.0},
            {imageControls.sceneExposureMultiplier, 0.0625, 16.0},
            {imageControls.bloomMultiplier, 0.0, 2.0}}};
        return std::all_of(ranges.begin(), ranges.end(), [](const auto& range) {
            return std::isfinite(range[0]) && range[0] >= range[1] && range[0] <= range[2];
        });
    }

    [[nodiscard]] bool MatrixValid() const noexcept
    {
        const auto& matrix = imageControls.colorBalanceBt709;
        return std::all_of(matrix.begin(), matrix.end(), [](double value) {
            return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
        });
    }
};

// Positive facts supplied by the controlled owner after a rendered frame closes.
struct DeveloperExposureFrameQualification
{
    bool developerMode{};
    bool rendered{};
    bool gameplayReady{};
    bool liveSettingsThreadCompatible{};
    [[nodiscard]] constexpr bool Eligible() const noexcept
    {
        return developerMode && rendered && gameplayReady && liveSettingsThreadCompatible;
    }
};

struct ScenePassConfiguration
{
    UINT width{};
    UINT height{};
    LayerWhiteConfiguration whites;
    // Borrowed only during Initialize; a requested invalid table fails initialization.
    const OperatorLutData* operatorLut{};
    SceneGradeConfiguration sceneGrade{};
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

struct NativeSceneHandles
{
    SceneTextureHandle input;
    SceneTextureHandle destination;
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

enum class NativeReferencePreparationScope { unavailable, qualifiedExclusive };

// One qualified immediate context, one initialization thread and one
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
        ID3D11DeviceContext* immediateContext, UINT width, UINT height) noexcept
    {
        return Initialize(device, immediateContext, ScenePassConfiguration{width, height, {}});
    }
    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, ScenePassConfiguration configuration) noexcept;
    // Caller excludes queries, command lists and concurrent/reentrant mutation.
    // Temporary draw bindings restore before return; borrowed retirement preserves
    // foreign context state. Caller-held scene views expire with this owner.
    [[nodiscard]] bool InitializeBorrowed(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, ScenePassConfiguration configuration) noexcept
    {
        if (!Initialize(device, immediateContext, configuration)) return false;
        borrowedContext_ = true;
        return true;
    }
    // Cold owner-thread resize first retires all owned resource views. A successful
    // native resize may rebuild this same borrowed context under a newer epoch;
    // failure stays unavailable. Permanent Retire cannot be restarted.
    [[nodiscard]] bool RetireForResize() noexcept;
    [[nodiscard]] bool ReinitializeBorrowed(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, ScenePassConfiguration configuration) noexcept;
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] bool Active() const noexcept
    {
        return OnOwnerThread() && (lifetime_.CurrentEpoch() & 1) != 0;
    }
    [[nodiscard]] bool MatchesContext(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext) const noexcept;
    [[nodiscard]] bool MatchesLayerWhites(LayerWhiteConfiguration whites) const noexcept
    {
        if (!Active() || invoking_) return false;
        return whites.uiWhiteNits == whites_.uiWhiteNits && whites.videoWhiteNits == whites_.videoWhiteNits;
    }

    HRESULT CreateObservedPixelShader(std::span<const std::byte> bytecode,
        ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept;
    // Bytecode is the successful native creation input for this same-device shader.
    [[nodiscard]] bool ObserveCreatedPixelShader(std::span<const std::byte> bytecode,
        ID3D11PixelShader* shader) noexcept;
    HRESULT CreateSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        const D3D11_SUBRESOURCE_DATA* initialData, SceneTextureHandle& handle) noexcept;
    // Cold caller supplies qualified scene storage; footprint alone proves no semantics.
    HRESULT AdoptSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        ID3D11Texture2D* texture, SceneTextureHandle& handle) noexcept;
    [[nodiscard]] bool CanPromoteNativeSceneTexture(const D3D11_TEXTURE2D_DESC& original) const noexcept;
    // Caller rechecks native admission after candidate preparation. This commit
    // moves private references without another native call or format inference.
    HRESULT AdoptPromotedNativeSceneTexture(NativeSceneTexture& prepared,
        SceneTextureHandle& handle) noexcept;
    // Explicit descriptor callers carry Execute's independent pass qualification.
    // Native callbacks instead use the observed-shader wrapper below.
    [[nodiscard]] NativeSceneHandles ResolveNativeSceneBindings(const RenoDxShaderDescriptor& descriptor) const noexcept;
    [[nodiscard]] NativeSceneHandles BoundNativeSceneHandles() const noexcept;
    [[nodiscard]] SceneTextureViews Views(SceneTextureHandle handle) const noexcept;

    [[nodiscard]] bool BeginFrame() noexcept;
    [[nodiscard]] bool EndFrame() noexcept;
    // Caller qualifies native look semantics and excludes active queries, command
    // lists and concurrent context mutation. The private result expires at close.
    [[nodiscard]] bool PrepareNativeReferenceLook(NativeReferencePreparationScope scope) noexcept;
    // Controlled diagnostic only: EndFrame alone never qualifies activation.
    // Save FrameGeneration before close; report eligibility once after that close.
    // The caller excludes active queries and command-list execution throughout
    // capture/replay; idle state getters cannot establish their absence.
    [[nodiscard]] bool CompleteExposureFrame(std::uint64_t savedFrame,
        DeveloperExposureFrameQualification qualification) noexcept;
    [[nodiscard]] bool SetExposureLockRequested(bool requested) noexcept;
    [[nodiscard]] bool ExposureLockActive() const noexcept;
    // Off by default. Owner-thread callers exclude queries, command lists and
    // concurrent mutation; the copied report is submission evidence only.
    [[nodiscard]] bool StartPipboyScreenObservation() noexcept;
    [[nodiscard]] bool StopPipboyScreenObservation() noexcept;
    [[nodiscard]] PipboyScreenObservation PipboyScreenSnapshot() const noexcept;
    // Query at consumption: retained handles remain storage-valid after lineage expires.
    [[nodiscard]] SceneTextureHandle CurrentScene() const noexcept;
    [[nodiscard]] SceneColorDomain CurrentSceneDomain() const noexcept;
    // Zero denotes a closed or unavailable frame; completed layers use this token.
    [[nodiscard]] std::uint64_t FrameGeneration() const noexcept
    {
        if (!Active() || invoking_) return 0;
        return frameOpen_ ? frameGeneration_ : 0;
    }
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
    class NativeReferenceSource
    {
    public:
        explicit NativeReferenceSource(ID3D11DeviceContext* context) noexcept;
        ~NativeReferenceSource() noexcept;
        void Bind(ID3D11ShaderResourceView* prepared) noexcept;
        NativeReferenceSource(const NativeReferenceSource&) = delete;
        NativeReferenceSource& operator=(const NativeReferenceSource&) = delete;
    private:
        ID3D11DeviceContext* context_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previous_;
        bool bound_{};
    };
    [[nodiscard]] HRESULT SceneRegistrationStatus(const D3D11_TEXTURE2D_DESC& original,
        const D3D11_SUBRESOURCE_DATA* initialData = nullptr) const noexcept;
    [[nodiscard]] bool CanObserveShader(ID3D11PixelShader* shader) const noexcept
    {
        return Active() && !invoking_ && shader != nullptr;
    }
    void ClearExclusiveState() noexcept
    {
        if (context_ && !borrowedContext_) context_->ClearState();
    }
    [[nodiscard]] bool ApplySettings(const OperatorLutData& table,
        LayerWhiteConfiguration whites, SceneGradeConfiguration sceneGrade = {}) noexcept;
    void MarkSettingsStale() noexcept;
    struct SceneTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResource;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
        std::uint64_t nativeCreationGeneration{};
        bool toneProduced{};
        bool operatorProduced{};
        bool settingsStale{};
        SceneColorDomain domain{SceneColorDomain::bt709Scene};
    };

    struct ObservedShader
    {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> original;
        const RenoDxShaderDescriptor* descriptor{};
        PipboyScreenShader pipboyRole{PipboyScreenShader::unknown};
    };

    enum class OriginalContinuation { none, retain, advance };

    struct DrawLineage
    {
        bool consumesSelected{};
        bool toneInput{};
        bool selectionExisted{};
        std::uint8_t writes{};
        SceneColorDomain domain{SceneColorDomain::bt709Scene};
        bool operatorProduced{};
        OriginalContinuation original{};
    };

    struct DrawReplacement
    {
        ID3D11PixelShader* shader{};
        ID3D11Buffer* videoConstants{};
        ID3D11Buffer* operatorConstants{};
        ID3D11PixelShader* radialFallback{};
        bool prepareNativeReference{};
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

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThread_ == std::this_thread::get_id();
    }
    [[nodiscard]] bool PrepareResources(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, ScenePassConfiguration configuration) noexcept;
    void ReleaseResources() noexcept;
    [[nodiscard]] bool CanResizeBorrowed() const noexcept;
    [[nodiscard]] bool CanInvoke(FiniteDrawInvocation invocation) const noexcept;
    [[nodiscard]] const SceneTexture* Resolve(SceneTextureHandle handle) const noexcept;
    [[nodiscard]] const RenoDxShaderDescriptor* BoundDescriptor() const noexcept;
    [[nodiscard]] bool PromotableScene(const D3D11_TEXTURE2D_DESC& original) const noexcept;
    [[nodiscard]] SceneTextureHandle NativeSceneForView(ID3D11View* view, bool input) const noexcept;
    [[nodiscard]] bool NativeSceneViewMatches(const SceneTexture& scene, ID3D11View* view, bool input) const noexcept;
    [[nodiscard]] bool QualifiedSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        ID3D11Texture2D* texture) const noexcept;
    [[nodiscard]] bool BoundSceneViewsMatch(const RenoDxShaderDescriptor& descriptor,
        const SceneTexture& source, const SceneTexture& target) const noexcept;
    [[nodiscard]] bool BoundTemporalViewsMatch(const RenoDxShaderDescriptor& descriptor,
        const SceneTexture& source, const SceneTexture& target) const noexcept;
    [[nodiscard]] std::uint8_t BoundWriteMask() const noexcept;
    void ClearLineage() noexcept;
    [[nodiscard]] bool PrepareNativeReferenceInputs() noexcept;
    [[nodiscard]] bool PrepareNativeReferenceDraw(const DrawReplacement& replacement) noexcept;
    void ConfigureNativeReferenceReplacement(DrawReplacement& replacement, RenoDxShaderPass pass) const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* NativeReferenceSourceForDraw(
        const DrawReplacement& replacement, bool prepared) const noexcept;
    void InvalidateSlot(std::size_t slot) noexcept;
    void InvalidateWrites(std::uint8_t writes) noexcept;
    [[nodiscard]] std::uint8_t WriteMaskForResource(ID3D11Resource* resource) const noexcept;
    [[nodiscard]] DrawLineage CaptureLineage(SceneTextureHandle input) const noexcept;
    [[nodiscard]] const SceneTexture* SelectedBoundInput(
        const RenoDxShaderDescriptor* descriptor, SceneTextureHandle input) const noexcept;
    [[nodiscard]] OriginalContinuation QualifyOriginalContinuation(
        const RenoDxShaderDescriptor* descriptor, SceneTextureHandle input,
        SceneTextureHandle destination) const noexcept;
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
    void RetirePipboyScreenObservation() noexcept;
    void ObservePipboyScreenDraw(SceneDrawOutcome outcome) noexcept;
    [[nodiscard]] PipboyScreenShader BoundPipboyScreenRole() const noexcept;
    void ObservePipboyScreenRole(PipboyScreenShader role) noexcept;
    void RejectPipboyScreenDraw(bool producer) noexcept;
    [[nodiscard]] bool CompletePipboyProducer(IUnknown* input) noexcept;
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
    bool borrowedContext_{};
    SceneTextureHandle selectedScene_;
    bool frameOpen_{};
    std::uint64_t frameGeneration_{};
    NativeReferenceResources nativeReference_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> nativeReferenceShader_;
    SceneGradeConfiguration sceneGrade_;
    std::uint64_t nativeReferenceFrame_{};
    DeveloperExposureLock exposureLock_;
    DeveloperExposureResources exposureResources_;
    std::uint64_t exposureGeneration_{};
    std::uint64_t endedExposureFrame_{};
    PipboyScreenObservation pipboyObservation_;
    Microsoft::WRL::ComPtr<IUnknown> pipboyProducerIdentity_;
};

}  // namespace fo4hdr
