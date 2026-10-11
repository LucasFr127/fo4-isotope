// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "ObservedDraw.hpp"
#include "../FrameTrace.hpp"
#include "ArmorCompositionPass.hpp"
#include "NativeReferenceGradePass.hpp"
#include "../resources/SceneResources.hpp"
#include "../resources/PresentationResources.hpp"
#include "../resources/ArmorCompositionResources.hpp"
#include "../resources/ReplacementShaders.hpp"
#include "../resources/DeveloperExposureResources.hpp"
#include "../resources/HdrResourcePolicy.hpp"
#include "../RendererLifetime.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LiveSettings.hpp"
#include "../OperatorConstants.hpp"
#include "../ShaderRegistry.hpp"
#include "../LutProfileBindings.hpp"
#include "../hooks/ContextHooks.hpp"
#include "../d3d11/ScopedD3D11PixelBindings.hpp"
#include "../d3d11/ScopedD3D11ComputeConstantBuffer.hpp"
#include <FO4Isotope/hooks/HookOwnership.hpp>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string_view>
#include "../SharedUiFrame.hpp"
#include "../SharedSdrFrame.hpp"

namespace fo4hdr {
namespace {
using Microsoft::WRL::ComPtr;

TemporalSceneEvidence ReadTemporalSceneEvidence(ID3D11DeviceContext* context,
    const DrawObservation& record) noexcept
{
    const auto exactProxyReady = IsPresentationProxyTarget(record.currentSecondaryRtv) &&
        g_presentation.presentationProxySrv != nullptr;
    const auto presentationTarget = exactProxyReady ||
        RenderTargetMatchesResource(record.currentSecondaryRtv,
            g_presentation.presentationBackBuffer.Get());
    const auto linearScene = g_scene.currentSceneDomain == LinearColorDomain::linearBt709Relative ||
        g_scene.currentSceneDomain == LinearColorDomain::linearBt2020UiRelative;
    return {.qualifiedSceneSelected = g_scene.currentScene != nullptr && linearScene,
        .consumesSelectedScene = CurrentPassConsumesSelectedScene(context, record),
        .presentationTarget = presentationTarget,
        .exactProxyReady = exactProxyReady,
        .internalSceneReady = record.currentReplacementActive && !presentationTarget &&
            IsFirstFp16RenderTarget(record.currentSecondaryRtv) &&
            IsQualifiedSceneTarget(record.currentSecondaryRtv)};
}

struct CompletedDrawObservation
{
    ID3D11DeviceContext* context;
    DrawObservation& record;
    bool productionOperatorApplied;
    SharedUiWriteEvidence write;

    PipelineHeartbeatEvaluation ObserveHeartbeat() const noexcept
    {
        g_heartbeat.Observe(record.currentPass);
        return g_heartbeat.Evaluate(
            g_evidence.resourceGeneration.load(std::memory_order_acquire), g_frame);
    }
    void ObserveLut() const noexcept
    {
        if (!record.currentReplacementActive) return;
        if (!IsQualifiedSceneTarget(record.currentPrimaryRtv)) return;
        auto* scene = AcquireSceneView(record.currentPrimaryRtv);
        if (scene == nullptr) return;
        const auto evaluation = ObserveHeartbeat();
        if (evaluation.ready)
        {
            // A new LUT write starts a new scene revision, even on a reused target.
            g_scene.currentScene = scene;
            g_scene.temporalPassObserved = false;
            g_scene.temporalSceneReady = false;
            g_scene.videoSceneReady = false;
            g_scene.productionOperatorApplied = productionOperatorApplied;
            g_scene.currentSceneDomain =
                productionOperatorApplied ?
                    LinearColorDomain::linearBt2020UiRelative :
                    LinearColorDomain::linearBt709Relative;
            if (IsPresentationProxyTarget(record.currentPrimaryRtv))
                g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
            g_evidence.colorDomain.store(
                static_cast<std::uint32_t>(g_scene.currentSceneDomain),
                std::memory_order_release);
            PublishSharedUiScene(write);
        }
    }

    void ObserveTemporal() const noexcept
    {
        // Both trusted Fallout 4 TAA variants write metadata/history to
        // SV_Target0 and filtered scene color to SV_Target1. An unrelated
        // temporal branch before the selected tone-to-LUT pair cannot impose
        // that branch's output contract on the eventual presentation scene.
        g_evidence.temporalAaDrawCount.fetch_add(
            1, std::memory_order_relaxed);
        const auto evaluation = ObserveHeartbeat();
        ApplyTemporalSceneAction(DecideTemporalSceneAction(evaluation,
            ReadTemporalSceneEvidence(context, record)));
    }

    void ApplyTemporalSceneAction(TemporalSceneAction action) const noexcept
    {
        switch (action)
        {
        case TemporalSceneAction::ignoreUnqualifiedBranch:
            g_evidence.temporalUnqualifiedDrawCount.fetch_add(
                1, std::memory_order_relaxed);
            return;
        case TemporalSceneAction::rejectMissingProxy:
            g_scene.temporalPassObserved = true;
            g_scene.temporalSceneReady = false;
            g_evidence.temporalMissingProxyDrawCount.fetch_add(
                1, std::memory_order_relaxed);
            return;
        case TemporalSceneAction::acceptProxy:
            g_scene.temporalPassObserved = true;
            g_scene.currentScene = g_presentation.presentationProxySrv;
            g_presentation.presentationProxyDomain =
                g_scene.currentSceneDomain;
            g_scene.temporalSceneReady = true;
            PublishSharedUiScene(write);
            g_evidence.temporalProxyDrawCount.fetch_add(
                1, std::memory_order_relaxed);
            return;
        case TemporalSceneAction::acceptInternalScene:
            g_scene.temporalPassObserved = true;
            g_scene.temporalSceneReady = false;
            if (auto* scene = AcquireSceneView(record.currentSecondaryRtv))
            {
                g_scene.currentScene = scene;
                g_scene.temporalSceneReady = true;
                PublishSharedUiScene(write);
            }
            return;
        }
    }

    void ObserveVideo() const noexcept
    {
            // Gameplay video is an explicit alternate source. Its replacement
            // maps independently configured video white into the UI-relative FP16
            // transport, but never masquerades as tone/LUT ownership.
            if (!record.currentReplacementActive) return;
            if (auto* videoScene =
                    AcquireSceneView(record.currentPrimaryRtv))
            {
                g_scene.currentScene = videoScene;
                g_scene.temporalPassObserved = false;
                g_scene.temporalSceneReady = false;
                g_scene.productionOperatorApplied = false;
                g_scene.currentSceneDomain =
                    LinearColorDomain::linearBt2020UiRelative;
                g_evidence.colorDomain.store(static_cast<std::uint32_t>(
                    g_scene.currentSceneDomain),
                    std::memory_order_release);
                g_scene.videoSceneReady = true;
                PublishSharedUiScene(write);
            }
    }

    void AdvanceOptionalScene() const noexcept
    {
        // A known optional pass advances only an exact selected scene input;
        // another full-aspect FP16 branch cannot become the presentation scene.
        if (!CurrentPassConsumesSelectedScene(context, record)) return;
        if (auto* nextScene = AcquireSceneView(record.currentPrimaryRtv))
        {
            g_scene.currentScene = nextScene;
            if (IsPresentationProxyTarget(record.currentPrimaryRtv))
                g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
            PublishSharedUiScene(write);
        }
    }

    bool IsSceneAdvancingPass() const noexcept
    {
        // DOF auxiliary blur/mask intermediates feed a later blender and do
        // not form presentation-scene boundaries themselves.
        constexpr std::array passes{RenoDxShaderPass::fxaa,
            RenoDxShaderPass::dofBlender, RenoDxShaderPass::gameDof,
            RenoDxShaderPass::vignette};
        return std::ranges::find(passes, record.currentPass) != passes.end();
    }

    void ObserveOther() const noexcept
    {
        if (!IsQualifiedSceneTarget(record.currentPrimaryRtv)) return;
        if (record.currentPass == RenoDxShaderPass::video)
        {
            ObserveVideo();
            return;
        }
        const auto evaluation = ObserveHeartbeat();
        if (!evaluation.ready) return;
        if (IsSceneAdvancingPass()) AdvanceOptionalScene();
    }

    bool IsReferenceObservationReady() const noexcept
    {
        if (g_armor.armorEncodedActive && context == g_rendererDevice.activeContext) return false;
        // Offscreen producers after the UI seed cannot replace its content or temporal proof.
        if (ApplicationPresentationFacadeActive() && SharedUiFrameEncoded()) return false;
        const std::array ready{record.currentPassKnown, HdrPathRequested(),
            g_referenceHealthy.load(), g_productionHealthy.load(),
            g_gameplayReadyApplied.load(std::memory_order_acquire)};
        return std::ranges::all_of(ready, [](bool value) { return value; });
    }
};

void ObserveCompletedDraw(ID3D11DeviceContext* context, DrawObservation& record,
    bool productionOperatorApplied, SharedUiWriteEvidence write) noexcept
{
    // Reference-pass validation must begin at the same render-thread lifecycle
    // boundary as shader substitution. Startup, video, loading, and menu draws
    // still use the original shaders/resources and cannot reject the later
    // gameplay generation merely because their TAA output is not promoted.
    const CompletedDrawObservation observation{context, record, productionOperatorApplied, write};
    if (!observation.IsReferenceObservationReady()) return;
    if (record.currentPass == RenoDxShaderPass::lut)
        observation.ObserveLut();
    else if (record.currentPass == RenoDxShaderPass::taa || record.currentPass == RenoDxShaderPass::taa2)
        observation.ObserveTemporal();
    else
        observation.ObserveOther();
}

} // namespace
using Microsoft::WRL::ComPtr;

[[nodiscard]] bool IsFirstSingleMip(const D3D11_SHADER_RESOURCE_VIEW_DESC& view) noexcept
{
    return view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        view.Texture2D.MostDetailedMip == 0 && view.Texture2D.MipLevels == 1;
}

[[nodiscard]] bool HdrFrameReadyForDraw() noexcept
{
    return HdrPathRequested() && g_referenceHealthy && g_productionHealthy &&
        g_gameplayReadyApplied.load(std::memory_order_acquire);
}



class ScopedExposureCaptureState
{
public:
    explicit ScopedExposureCaptureState(
        ID3D11DeviceContext* context) noexcept : context_(context)
    {
        try
        {
            if (context_ == nullptr) return;
            CaptureShader();
            CaptureBindings();
            active_ = true;
        }
        catch (...) { RecordAbiException(); }
    }

    ~ScopedExposureCaptureState() noexcept
    {
        try
        {
            if (!active_) return;
            RestoreBindings();
            RestoreShader();
        }
        catch (...)
        {
            g_referenceHealthy = false;
            RecordAbiException();
        }
    }

    ScopedExposureCaptureState(
        const ScopedExposureCaptureState&) = delete;
    ScopedExposureCaptureState& operator=(
        const ScopedExposureCaptureState&) = delete;

    [[nodiscard]] bool Active() const noexcept { return active_; }

private:
    void CaptureShader()
    {
        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES>
            instances{};
        instanceCount_ = static_cast<UINT>(instances.size());
        ID3D11ComputeShader* shader{};
        context_->CSGetShader(&shader, instances.data(), &instanceCount_);
        shader_.Attach(shader);
        for (UINT index = 0; index < instanceCount_; ++index)
            instances_[index].Attach(instances[index]);
    }

    void CaptureBindings()
    {
        ID3D11Buffer* exposureConstant{};
        context_->CSGetConstantBuffers(2, 1, &exposureConstant);
        exposureConstant_.Attach(exposureConstant);
        ID3D11ShaderResourceView* resource{};
        context_->CSGetShaderResources(0, 1, &resource);
        resource_.Attach(resource);
        ID3D11UnorderedAccessView* unordered{};
        context_->CSGetUnorderedAccessViews(0, 1, &unordered);
        unordered_.Attach(unordered);
    }

    void RestoreBindings()
    {
        ID3D11ShaderResourceView* clearResource{};
        ID3D11UnorderedAccessView* clearUnordered{};
        context_->CSSetShaderResources(0, 1, &clearResource);
        context_->CSSetUnorderedAccessViews(0, 1, &clearUnordered, nullptr);

        auto* resource = resource_.Get();
        auto* unordered = unordered_.Get();
        auto* exposureConstant = exposureConstant_.Get();
        context_->CSSetConstantBuffers(2, 1, &exposureConstant);
        context_->CSSetShaderResources(0, 1, &resource);
        context_->CSSetUnorderedAccessViews(0, 1, &unordered, nullptr);
    }

    void RestoreShader()
    {
        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES>
            instances{};
        for (UINT index = 0; index < instanceCount_; ++index)
            instances[index] = instances_[index].Get();
        context_->CSSetShader(shader_.Get(),
            instanceCount_ == 0 ? nullptr : instances.data(),
            instanceCount_);
    }

    ID3D11DeviceContext* context_{};
    ComPtr<ID3D11ComputeShader> shader_;
    std::array<ComPtr<ID3D11ClassInstance>,
        D3D11_SHADER_MAX_INTERFACES> instances_{};
    UINT instanceCount_{};
    ComPtr<ID3D11Buffer> exposureConstant_;
    ComPtr<ID3D11ShaderResourceView> resource_;
    ComPtr<ID3D11UnorderedAccessView> unordered_;
    bool active_{};
};

void ClearExposureCaptureViews(
    ID3D11DeviceContext* context) noexcept
{
    std::array<ID3D11ShaderResourceView*, 1> resources{};
    std::array<ID3D11UnorderedAccessView*, 1> unordered{};
    context->CSSetShaderResources(0,
        static_cast<UINT>(resources.size()), resources.data());
    context->CSSetUnorderedAccessViews(0,
        static_cast<UINT>(unordered.size()), unordered.data(), nullptr);
}

[[nodiscard]] bool IsExposureCaptureSafe(
    ID3D11DeviceContext* context, const DrawObservation& record)
{
    if (context == nullptr || context != g_rendererDevice.activeContext) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !record.computeQueries.CanDispatch())
        return false;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue{};
    context->GetPredication(&predicate, &predicateValue);
    return !predicate;
}

struct NativeExposureBinding
{
    ComPtr<ID3D11ShaderResourceView> exposure;
    ComPtr<ID3D11Buffer> constants;
    UINT first{};
    UINT count{};
};

[[nodiscard]] bool IsNativeExposureTexture(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    const std::array dimensions{description.Width, description.Height, description.MipLevels,
        description.ArraySize, description.SampleDesc.Count};
    return std::ranges::all_of(dimensions, [](UINT dimension) { return dimension == 1; });
}

[[nodiscard]] bool IsNativeExposureFormat(DXGI_FORMAT format) noexcept
{
    constexpr std::array formats{DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16_FLOAT,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    return std::ranges::find(formats, format) != formats.end();
}

[[nodiscard]] bool ValidExposureConstantRange(UINT first, UINT count, UINT width) noexcept
{
    return count >= 2 && static_cast<std::uint64_t>(first) * 16U + 32U <= width;
}

[[nodiscard]] bool MatchesNativeExposure(const D3D11_SHADER_RESOURCE_VIEW_DESC& view,
    const D3D11_TEXTURE2D_DESC& description, UINT bufferWidth) noexcept
{
    return IsFirstSingleMip(view) && IsNativeExposureTexture(description) &&
        bufferWidth >= 32 && IsNativeExposureFormat(view.Format);
}

[[nodiscard]] bool QualifiedNativeExposure(
    ID3D11DeviceContext* context, NativeExposureBinding& binding) noexcept
{
    auto* context1 = g_developerExposure.context1.Get();
    UINT first{}, count{};
    context->PSGetShaderResources(2, 1, &binding.exposure);
    if (context1) context1->PSGetConstantBuffers1(2, 1, &binding.constants, &first, &count);
    else context->PSGetConstantBuffers(2, 1, &binding.constants);
    if (!binding.exposure || !binding.constants) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    binding.exposure->GetDesc(&view);
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    binding.exposure->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    D3D11_BUFFER_DESC buffer{};
    binding.constants->GetDesc(&buffer);
    if (context1 && !ValidExposureConstantRange(first, count, buffer.ByteWidth)) return false;
    binding.first = first;
    binding.count = count;
    return MatchesNativeExposure(view, description, buffer.ByteWidth);
}

[[nodiscard]] bool CaptureQualifiedNativeExposure(
    ID3D11DeviceContext* context, const DrawObservation& record,
    ID3D11ComputeShader* shader, ID3D11UnorderedAccessView* output) noexcept
{
    if (!shader || !output) return false;
    if (!IsExposureCaptureSafe(context, record))
        return false;
    NativeExposureBinding binding;
    if (!QualifiedNativeExposure(context, binding)) return false;
    auto* context1 = g_developerExposure.context1.Get();
    // Restore the complete original b2 range after the general compute guard's
    // whole-buffer restore; the range guard therefore outlives that guard.
    ScopedD3D11ComputeConstantBuffer exposureRange;
    exposureRange.Capture(context, context1, 2);
    ScopedExposureCaptureState saved(context);
    if (!saved.Active()) return false;
    auto* source = binding.exposure.Get();
    auto* bufferPointer = binding.constants.Get();
    context->CSSetShaderResources(0, 1, &source);
    if (context1) context1->CSSetConstantBuffers1(2, 1, &bufferPointer, &binding.first, &binding.count);
    else context->CSSetConstantBuffers(2, 1, &bufferPointer);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(shader, nullptr, 0);
    context->Dispatch(1, 1, 1);
    ClearExposureCaptureViews(context);
    return true;
}

void ObserveDeveloperExposure(
    ID3D11DeviceContext* context, const DrawObservation& record) noexcept
{
    auto& diagnostic = g_developerExposure;
    if (!g_configuration.developerMode) return;
    if (!diagnostic.state.Requested() && !diagnostic.state.Active()) return;
    NativeExposureBinding binding;
    if (!QualifiedNativeExposure(context, binding))
    {
        // Retire at Present to keep the frame's exposure coherent.
        diagnostic.state.Request(false);
        diagnostic.failed = true;
        return;
    }
    diagnostic.observedFrame = g_frame;
    if (diagnostic.state.NeedsCapture() && CaptureQualifiedNativeExposure(
            context, record, diagnostic.captureShader.Get(), diagnostic.uav.Get()))
        diagnostic.state.Capture(g_frame,
            g_evidence.resourceGeneration.load(std::memory_order_acquire));
}



struct ProductionDrawKind
{
    bool lut{};
    bool toneMap{};
    bool temporal{};
    bool video{};
    [[nodiscard]] bool Supported() const noexcept { return toneMap || lut || temporal || video; }
};

[[nodiscard]] ProductionDrawKind ClassifyProductionDraw(RenoDxShaderPass pass) noexcept
{
    constexpr std::array tonePasses{RenoDxShaderPass::toneMapA, RenoDxShaderPass::toneMapB,
        RenoDxShaderPass::toneMapNewA, RenoDxShaderPass::toneMapNewB};
    return {pass == RenoDxShaderPass::lut,
        std::ranges::find(tonePasses, pass) != tonePasses.end(),
        pass == RenoDxShaderPass::taa || pass == RenoDxShaderPass::taa2,
        pass == RenoDxShaderPass::video};
}

[[nodiscard]] bool ProductionResourcesReady() noexcept
{
    return HdrFrameReadyForDraw() && g_replacements.device == g_rendererDevice.activeDevice &&
        ReplacementConstantBuffersReady();
}

[[nodiscard]] bool IsProductionScene(const DrawObservation& record,
    const PipelineHeartbeatEvaluation& heartbeat) noexcept
{
    return IsQualifiedSceneTarget(record.currentPrimaryRtv) &&
        heartbeat.currentGeneration && heartbeat.currentFrame;
}

[[nodiscard]] bool IsProductionTemporal(ID3D11DeviceContext* context, const DrawObservation& record,
    const PipelineHeartbeatEvaluation& heartbeat) noexcept
{
    if (!g_scene.productionOperatorApplied) return false;
    const auto action = DecideTemporalSceneAction(heartbeat, ReadTemporalSceneEvidence(context, record));
    return action == TemporalSceneAction::acceptProxy ||
        action == TemporalSceneAction::acceptInternalScene;
}

[[nodiscard]] bool IsProductionDraw(ID3D11DeviceContext* context, const DrawObservation& record,
    bool configurationMatches, const PipelineHeartbeatEvaluation& heartbeat) noexcept
{
    if (!configurationMatches || !g_configuration.productionRequested) return false;
    const auto kind = ClassifyProductionDraw(record.currentPass);
    if (kind.toneMap) return IsProductionScene(record, heartbeat);
    if (kind.lut) return IsProductionScene(record, heartbeat) && heartbeat.toneMapFamilyObserved;
    if (kind.temporal)
    {
        // Unrelated TAA branches bind neutral b13; only selected scene lineage
        // may undo and restore production UI-relative transport around history.
        return IsProductionTemporal(context, record, heartbeat);
    }
    // Video uses the fixed b13 layout without claiming the gameplay heartbeat.
    return true;
}

class ScopedProductionBindings
{
public:
    ScopedProductionBindings(
        ID3D11DeviceContext* context,
        const DrawObservation& record) noexcept : context_(context)
    {
        const auto kind = ClassifyProductionDraw(record.currentPass);
        if (context_ == nullptr) return;
        if (!record.currentPassKnown || !record.currentReplacementActive) return;
        if (!kind.Supported() || !ProductionResourcesReady()) return;
        const auto configurationMatches = ReplacementSetMatchesConfiguration();
        const auto heartbeat = g_heartbeat.Evaluate(
            g_evidence.resourceGeneration.load(std::memory_order_acquire), g_frame);
        lut_ = kind.lut;
        production_ = IsProductionDraw(context_, record, configurationMatches, heartbeat);
        BindProductionInputs(record, kind.toneMap);
    }

    ~ScopedProductionBindings() noexcept = default;

    ScopedProductionBindings(const ScopedProductionBindings&) = delete;
    ScopedProductionBindings& operator=(
        const ScopedProductionBindings&) = delete;

    [[nodiscard]] bool UsesProductionSettings() const noexcept
    {
        return active_ && production_;
    }
    [[nodiscard]] bool UsesProductionOperator() const noexcept
    {
        return active_ && production_ && lut_;
    }

private:
    void BindProductionInputs(const DrawObservation& record, bool toneMap) noexcept
    {
        if (production_ && toneMap) ObserveDeveloperExposure(context_, record);
        auto* owned = SelectConstants(record);
        const bool lockExposure = ShouldLockExposure(toneMap);
        if (lockExposure) owned = g_developerExposure.constants.Get();
        const auto bindOperatorLut = production_ && lut_ &&
            RequiresOperatorLut(g_replacements.configurationKey.mode);
        active_ = bindings_.Bind(context_, g_rendererDevice.activeContext1.Get(),
            {.constantBufferSlot = kProductionOperatorConstantSlot, .constantBuffer = owned,
            .shaderResourceSlot = kProductionOperatorLutSlot,
            .shaderResource = g_replacements.operatorLutSrv.Get(),
            .samplerSlot = kProductionOperatorSamplerSlot,
            .sampler = g_replacements.operatorLutSampler.Get(), .bindShaderResource = bindOperatorLut,
            .extraShaderResourceSlot = 17,
            .extraShaderResource = lockExposure ? g_developerExposure.srv.Get() : nullptr,
            .rejectExtraShaderResourceCollision = false});
        if (!active_) BindFallback();
    }

    [[nodiscard]] ID3D11Buffer* SelectConstants(const DrawObservation& record) noexcept
    {
        ID3D11Buffer* owned = production_ ?
            g_replacements.operatorConstants.Get() :
            g_replacements.bypassConstants.Get();
        if (production_ && lut_) owned = PrepareLutProfileDraw(context_, record, owned);
        return owned;
    }

    [[nodiscard]] bool ShouldLockExposure(bool toneMap) const noexcept
    {
        return production_ && toneMap && g_developerExposure.state.Active();
    }

    void BindFallback() noexcept
    {
        MarkWeatherResponseBypassed();
        g_productionHealthy = false;
        RecordFailure(GraphicsObservationFailure::requiredHeartbeatMissing);
        production_ = false;
        // This draw must not sample the operator LUT with incomplete
        // bindings: mode 0 uses only the
        // original scene/look resources. Context, bypass buffer and the
        // inactive guard were verified above; constants-only Bind has no
        // remaining failure condition or fallible D3D call.
        active_ = bindings_.Bind(context_, g_rendererDevice.activeContext1.Get(),
            {.constantBufferSlot = kProductionOperatorConstantSlot,
            .constantBuffer = g_replacements.bypassConstants.Get(),
            .shaderResourceSlot = kProductionOperatorLutSlot, .shaderResource = nullptr,
            .samplerSlot = kProductionOperatorSamplerSlot, .sampler = nullptr,
            .bindShaderResource = false});
        assert(active_);
        // Earlier HDR scene work cannot be undone within this draw. The
        // current proxy receives the bounded SDR handoff, not a claim of
        // exact vanilla reconstruction for the partial transition frame.
    }

    ID3D11DeviceContext* context_{};
    ScopedD3D11PixelBindings bindings_;
    bool active_{};
    bool production_{};
    bool lut_{};
};

class ScopedNativeReferenceDraw
{
public:
    ScopedNativeReferenceDraw(ID3D11DeviceContext* context, DrawObservation& record,
        bool production) noexcept : context_(context), record_(record)
    {
        if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire))
            return;
        if (!Eligible(production)) return;
        const auto& resources = g_replacements.nativeReference;
        if (!ResourcesReady(resources)) return;
        const bool prepare = g_replacements.configurationKey.vanillaColorGradingStrength > 0.0F;
        auto* context1 = g_rendererDevice.activeContext1.Get();
        if (!PreparationAvailable(prepare, context1)) return;
        ComPtr<ID3D11PixelShader> actual;
        if (!BindShader(resources, actual)) return;
        if (!Prepare(prepare, context1, resources)) return;
        g_nativeReferenceStatus = nullptr;
    }

    ~ScopedNativeReferenceDraw() noexcept
    {
        if (preparedBound_)
        {
            auto* previous = inputs_.looks[0].Get();
            context_->PSSetShaderResources(3, 1, &previous);
        }
        RestoreShader();
    }

    ScopedNativeReferenceDraw(const ScopedNativeReferenceDraw&) = delete;
    ScopedNativeReferenceDraw& operator=(const ScopedNativeReferenceDraw&) = delete;

private:
    [[nodiscard]] bool Eligible(bool production) const noexcept
    {
        return g_activeSettings.colorGradingMethod == ColorGradingMethod::nativeReference &&
            production && record_.currentPass == RenoDxShaderPass::lut &&
            record_.currentReplacementActive && HasOriginalPixelShaderSetter(context_);
    }

    [[nodiscard]] static bool ResourcesReady(const NativeReferenceResources& resources) noexcept
    {
        const bool ready = NativeReferenceResourcesReady(resources);
        if (!ready) g_nativeReferenceStatus = "Radial fallback: grading resources unavailable.";
        return ready;
    }

    [[nodiscard]] bool CanPrepare(ID3D11DeviceContext1* context1) const noexcept
    {
        return context_ == g_rendererDevice.activeContext && record_.immediate && context1 &&
            record_.computeQueries.CanDispatch();
    }

    [[nodiscard]] bool PreparationAvailable(bool prepare, ID3D11DeviceContext1* context1) const noexcept
    {
        if (!prepare || CanPrepare(context1)) return true;
        g_nativeReferenceStatus = "Radial fallback: preparation is unavailable for this draw.";
        return false;
    }
    [[nodiscard]] bool BindShader(const NativeReferenceResources& resources,
        ComPtr<ID3D11PixelShader>& actual) noexcept
    {
        BindOriginalPixelShader(context_, resources.shader.Get());
        swapped_ = true;

        context_->PSGetShader(&actual, nullptr, nullptr);
        if (actual.Get() != resources.shader.Get())
        {
            RestoreShader();
            g_nativeReferenceStatus = "Radial fallback: grading shader binding changed.";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Prepare(bool prepare, ID3D11DeviceContext1* context1,
        const NativeReferenceResources& resources) noexcept
    {
        if (!prepare) return true;
        // Recheck after the captured shader-binding call: another hook may
        // change predication, query state or the native input bindings.
        if (!record_.computeQueries.CanDispatch() ||
            !CaptureNativeReferenceInputs(context_, context1, inputs_))
        {
            RestoreShader();
            g_nativeReferenceStatus = "Radial fallback: native LUT inputs or draw state are unsupported.";
            return false;
        }
        DispatchNativeReferencePrepare(context_, context1, inputs_, resources);
        auto* prepared = resources.preblendSrv.Get();
        context_->PSSetShaderResources(3, 1, &prepared);
        preparedBound_ = true;
        return true;
    }

    void RestoreShader() noexcept
    {
        if (!swapped_) return;
        BindOriginalPixelShader(context_, record_.expectedBoundShader);
        (void)ValidateExpectedShaderBinding(context_, record_);
        swapped_ = false;
    }

    ID3D11DeviceContext* context_{};
    DrawObservation& record_;
    NativeReferenceInputs inputs_;
    bool swapped_{};
    bool preparedBound_{};
};

void CloseConflictingArmorDraw(const DrawObservation& record, bool sharedUiRejected) noexcept
{
    if (g_armor.armorEncodedSharedSrgb ? sharedUiRejected : record.currentReplacementActive)
    {
        ArmorUiScopeToken outer{};
        outer.serial = g_armor.armorEncodedScope;
        ArmorEncodedBoundary(outer, false);
        if (!g_armor.armorEncodedSharedSrgb) g_armor.armorEncodedFailed = true;
        if (g_referenceHealthy)
            g_armor.armorEncodedStatus = "Rejected frame: conflicting draw inside armor scope";
    }
}

void RecordProductionDraw(const ScopedProductionBindings& operatorBindings) noexcept
{
    if (operatorBindings.UsesProductionSettings())
        ObserveQualifiedSettingsDrawThread();
    if (operatorBindings.UsesProductionOperator())
    {
        g_evidence.productionOperatorDrawCount.fetch_add(
            1, std::memory_order_relaxed);
    }
}

void ObserveCompletedNativeDraw(ID3D11DeviceContext* context, const DrawObservation& record) noexcept
{
    ObservePipboyScreenDraw(context, record);
    ObserveSharedSdrDraw(context, record);
}

void ExecuteObservedDraw(
    DrawObservation& record,
    ID3D11DeviceContext* context,
    const DrawInvocation& draw) noexcept
{
    if (context != g_rendererDevice.activeContext)
    {
        draw();
        return;
    }
    const bool expectedShader = ValidateExpectedShaderBinding(context, record);
    ObserveSharedSdrDrawEntry(context);
    const bool sharedUiRejected = ObserveSharedUiDraw(record);
    if (g_armor.armorEncodedActive)
    {
        CloseConflictingArmorDraw(record, sharedUiRejected);
        draw();
        ObserveCompletedNativeDraw(context, record);
        return;
    }
    if (!expectedShader)
    {
        draw();
        ObserveCompletedNativeDraw(context, record);
        return;
    }
    bool productionOperatorApplied{};
    SharedUiWriteEvidence write;
    {
        ScopedProductionBindings operatorBindings(context, record);
        write = CaptureSharedUiWrite(record.currentPass == RenoDxShaderPass::lut &&
            operatorBindings.UsesProductionOperator(), operatorBindings.UsesProductionSettings() ?
                g_replacements.configurationKey.uiTransportWhiteNits : 0.0F);
        {
            ScopedNativeReferenceDraw grading(context, record,
                operatorBindings.UsesProductionSettings());
            draw();
        }
        ObserveCompletedNativeDraw(context, record);
        RecordProductionDraw(operatorBindings);
        productionOperatorApplied = operatorBindings.UsesProductionOperator();
    }
    try { ObserveCompletedDraw(context, record, productionOperatorApplied, write); }
    catch (...) { RecordAbiException(); }
}


} // namespace fo4hdr
