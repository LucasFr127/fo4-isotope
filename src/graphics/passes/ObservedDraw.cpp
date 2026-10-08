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

namespace fo4hdr {
namespace {
using Microsoft::WRL::ComPtr;

struct CompletedDrawObservation
{
    ID3D11DeviceContext* context;
    DrawObservation& record;

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
            // The first resource-qualified tone-to-LUT branch is the common
            // seam. Other image-space branches in the same frame are not
            // interpreted as contradictory global ordering evidence.
            g_scene.currentScene = scene;
            g_scene.currentSceneDomain =
                g_configuration.productionRequested ?
                    LinearColorDomain::linearBt2020UiRelative :
                    LinearColorDomain::linearBt709Relative;
            g_evidence.colorDomain.store(
                static_cast<std::uint32_t>(g_scene.currentSceneDomain),
                std::memory_order_release);
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
        const auto exactProxyReady =
            record.currentSecondaryRtv != nullptr &&
            record.currentSecondaryRtv ==
                g_presentation.presentationProxyRtv.Get() &&
            g_presentation.presentationProxySrv != nullptr;
        const auto consumesSelectedScene =
            CurrentPassConsumesSelectedScene(context, record);
        const auto presentationTarget = exactProxyReady ||
            RenderTargetMatchesResource(record.currentSecondaryRtv,
                g_presentation.presentationBackBuffer.Get());
        ApplyTemporalSceneAction(DecideTemporalSceneAction(evaluation,
                    {.qualifiedSceneSelected = g_scene.currentScene != nullptr,
                    .consumesSelectedScene = consumesSelectedScene,
                    .presentationTarget = presentationTarget,
                    .exactProxyReady = exactProxyReady}));
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
            g_evidence.temporalMissingProxyDrawCount.fetch_add(
                1, std::memory_order_relaxed);
            return;
        case TemporalSceneAction::acceptProxy:
            g_scene.temporalPassObserved = true;
            g_scene.currentScene = g_presentation.presentationProxySrv;
            g_presentation.presentationProxyDomain =
                g_scene.currentSceneDomain;
            g_scene.temporalSceneReady = true;
            g_evidence.temporalProxyDrawCount.fetch_add(
                1, std::memory_order_relaxed);
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
                g_scene.currentSceneDomain =
                    LinearColorDomain::linearBt2020UiRelative;
                g_evidence.colorDomain.store(static_cast<std::uint32_t>(
                    g_scene.currentSceneDomain),
                    std::memory_order_release);
                g_scene.videoSceneReady = true;
            }
    }

    void AdvanceOptionalScene() const noexcept
    {
        // A known optional pass advances only an exact selected scene input;
        // another full-aspect FP16 branch cannot become the presentation scene.
        if (!CurrentPassConsumesSelectedScene(context, record)) return;
        if (auto* nextScene = AcquireSceneView(record.currentPrimaryRtv))
            g_scene.currentScene = nextScene;
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
        return record.currentPassKnown && HdrPathRequested() &&
            g_referenceHealthy && g_gameplayReadyApplied.load(std::memory_order_acquire);
    }
};

void ObserveCompletedDraw(ID3D11DeviceContext* context, DrawObservation& record) noexcept
{
    if (g_armor.armorEncodedActive && context == g_rendererDevice.activeContext) return;
    // Reference-pass validation must begin at the same render-thread lifecycle
    // boundary as shader substitution. Startup, video, loading, and menu draws
    // still use the original shaders/resources and cannot reject the later
    // gameplay generation merely because their TAA output is not promoted.
    const CompletedDrawObservation observation{context, record};
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

[[nodiscard]] bool IsSingleSampledShaderTexture(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    return description.ArraySize == 1 && description.SampleDesc.Count == 1 &&
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

[[nodiscard]] bool IsFirstSingleMip(const D3D11_SHADER_RESOURCE_VIEW_DESC& view) noexcept
{
    return view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        view.Texture2D.MostDetailedMip == 0 && view.Texture2D.MipLevels == 1;
}

[[nodiscard]] bool IsFullResolutionSdrUiTexture(const D3D11_TEXTURE2D_DESC& description,
    const D3D11_SHADER_RESOURCE_VIEW_DESC& view) noexcept
{
    return description.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        view.Format == DXGI_FORMAT_R8G8B8A8_UNORM && IsFirstSingleMip(view) &&
        description.Width == g_presentation.width && description.Height == g_presentation.height &&
        IsSingleSampledShaderTexture(description);
}

[[nodiscard]] bool HasFullResolutionSdrUiSource(
    ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr) return false;
    if (g_presentation.width == 0 || g_presentation.height == 0)
        return false;
    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(0, 1, &source);
    if (!source) return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    source->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    source->GetDesc(&viewDescription);
    return IsFullResolutionSdrUiTexture(description, viewDescription);
}

[[nodiscard]] bool HasTexture2dUiMaskSource(
    ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr) return false;
    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(0, 1, &source);
    if (!source) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    source->GetDesc(&viewDescription);
    if (viewDescription.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    source->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return description.Width != 0 && description.Height != 0 && IsSingleSampledShaderTexture(description);
}

enum class NativeUiCaptureKind : std::uint8_t
{
    none,
    backdrop,
    composite
};

[[nodiscard]] bool HdrFrameReadyForDraw() noexcept
{
    return HdrPathRequested() && g_referenceHealthy &&
        g_gameplayReadyApplied.load(std::memory_order_acquire);
}

class ScopedGameUiCapture
{
public:
    ScopedGameUiCapture(
        ID3D11DeviceContext* context,
        DrawObservation& record,
        bool exactIndexedQuad) noexcept : context_(context), record_(&record)
    {
        if (!CanCaptureUiFrame() || !HasCaptureTargets()) return;

        const auto allowlistedUiShader =
            IsLateGameUiShader(record.expectedBoundShader);
        const auto allowlistedBackdropShader =
            IsLateGameUiBackdropShader(record.expectedBoundShader);
        if (allowlistedUiShader == allowlistedBackdropShader)
            return;

        ComPtr<ID3D11PixelShader> actualShader;
        context_->PSGetShader(&actualShader, nullptr, nullptr);
        if (actualShader.Get() != record.expectedBoundShader)
            return;

        if (!CapturePreviousTargets()) return;
        if (!SelectCaptureKind(allowlistedUiShader, exactIndexedQuad)) return;
        CaptureBlendState();
        if (!BindCaptureTarget()) return;
        ActivateCapture();
    }

    ~ScopedGameUiCapture() noexcept
    {
        if (!active_) return;
        if (context_ == nullptr || record_ == nullptr) return;
        context_->OMSetBlendState(previousBlend_.Get(),
            blendFactor_.data(), sampleMask_);
        ID3D11RenderTargetView* restore = previousRtvs_[0].Get();
        InvokeCapturedNext(OriginalRenderTargetSetter(context_), context_, 1,
            &restore, depthStencil_.Get());
    }

    ScopedGameUiCapture(const ScopedGameUiCapture&) = delete;
    ScopedGameUiCapture& operator=(const ScopedGameUiCapture&) = delete;

    [[nodiscard]] bool Active() const noexcept { return active_; }
    [[nodiscard]] NativeUiCaptureKind Kind() const noexcept { return kind_; }

private:
    void ActivateCapture() noexcept
    {
        context_->OMSetBlendState(
            g_uiTransport.gameUiBlend.Get(), nullptr, sampleMask_);
        if (kind_ == NativeUiCaptureKind::backdrop)
            g_uiTransport.gameUiBackdropCapturedThisFrame = true;
        else
            g_uiTransport.gameUiCapturedThisFrame = true;
        active_ = true;
    }

    [[nodiscard]] static bool CanCaptureUiFrame() noexcept
    {
        return g_configuration.productionRequested && HdrFrameReadyForDraw() &&
            g_scene.productionOperatorApplied && g_scene.currentScene != nullptr;
    }

    [[nodiscard]] bool HasCaptureTargets() const noexcept
    {
        return record_->currentSinglePresentationTarget && context_ != nullptr &&
            OriginalRenderTargetSetter(context_) != nullptr && UiCaptureResourcesReady();
    }

    bool CapturePreviousTargets() noexcept
    {
        std::array<ID3D11RenderTargetView*,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> current{};
        ID3D11DepthStencilView* depth{};
        context_->OMGetRenderTargets(static_cast<UINT>(current.size()),
            current.data(), &depth);
        for (std::size_t index = 0; index < current.size(); ++index)
            previousRtvs_[index].Attach(current[index]);
        depthStencil_.Attach(depth);
        if (previousRtvs_[0].Get() !=
                g_presentation.presentationProxyRtv.Get() ||
            std::any_of(previousRtvs_.begin() + 1, previousRtvs_.end(),
                [](const auto& view) { return view != nullptr; }))
            return false;

        std::array<ID3D11UnorderedAccessView*,
            D3D11_PS_CS_UAV_REGISTER_COUNT> uavs{};
        context_->OMGetRenderTargetsAndUnorderedAccessViews(
            0, nullptr, nullptr, 0, static_cast<UINT>(uavs.size()),
            uavs.data());
        bool uavBound{};
        for (auto* uav : uavs)
        {
            uavBound |= uav != nullptr;
            if (uav != nullptr) uav->Release();
        }
        return !uavBound;
    }

    bool SelectCaptureKind(bool allowlistedUiShader, bool exactIndexedQuad) noexcept
    {
        const LateGameUiFrame commonArguments{
            .hdrPathRequested = HdrPathRequested(), .referenceHealthy = g_referenceHealthy.load(),
            .gameplayReady = g_gameplayReadyApplied.load(std::memory_order_acquire),
            .productionOperatorApplied = g_scene.productionOperatorApplied,
            .qualifiedSceneSelected = g_scene.currentScene != nullptr,
            .exactSinglePresentationTarget = record_->currentSinglePresentationTarget};
        if (allowlistedUiShader)
        {
            if (DecideLateGameUiAction(commonArguments, true,
                    HasFullResolutionSdrUiSource(context_)) !=
                LateGameUiAction::captureSeparately)
                return false;
            kind_ = NativeUiCaptureKind::composite;
        }
        else
        {
            if (DecideLateGameUiBackdropAction(commonArguments,
                    {.allowlistedBackdropShader = true, .exactIndexedQuad = exactIndexedQuad,
                    .texture2dMaskSource = HasTexture2dUiMaskSource(context_),
                    .depthDetached = depthStencil_ == nullptr}) !=
                LateGameUiAction::captureSeparately)
                return false;
            kind_ = NativeUiCaptureKind::backdrop;
        }

        return true;
    }

    void CaptureBlendState() noexcept
    {
        FLOAT factor[4]{};
        UINT sampleMask{};
        ID3D11BlendState* blend{};
        context_->OMGetBlendState(&blend, factor, &sampleMask);
        previousBlend_.Attach(blend);
        std::copy(std::begin(factor), std::end(factor), blendFactor_.begin());
        sampleMask_ = sampleMask;
    }

    bool BindCaptureTarget() noexcept
    {
        auto* target = kind_ == NativeUiCaptureKind::backdrop ?
            g_uiTransport.gameUiBackdropRtv.Get() :
            g_uiTransport.gameUiRtv.Get();
        const auto alreadyCaptured =
            kind_ == NativeUiCaptureKind::backdrop ?
            g_uiTransport.gameUiBackdropCapturedThisFrame :
            g_uiTransport.gameUiCapturedThisFrame;
        if (!alreadyCaptured)
        {
            constexpr float transparent[4]{};
            context_->ClearRenderTargetView(target, transparent);
        }
        InvokeCapturedNext(OriginalRenderTargetSetter(context_), context_, 1,
            &target, depthStencil_.Get());
        std::array<ID3D11RenderTargetView*,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> installed{};
        context_->OMGetRenderTargets(static_cast<UINT>(installed.size()),
            installed.data(), nullptr);
        const auto installedOwned = installed[0] == target &&
            std::all_of(installed.begin() + 1, installed.end(),
                [](const auto* view) { return view == nullptr; });
        for (auto* view : installed)
            if (view != nullptr) view->Release();
        if (!installedOwned)
        {
            ID3D11RenderTargetView* restore = previousRtvs_[0].Get();
            InvokeCapturedNext(OriginalRenderTargetSetter(context_), context_, 1,
                &restore, depthStencil_.Get());
            return false;
        }

        return true;
    }

    ID3D11DeviceContext* context_{};
    DrawObservation* record_{};
    std::array<ComPtr<ID3D11RenderTargetView>,
        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> previousRtvs_{};
    ComPtr<ID3D11DepthStencilView> depthStencil_;
    ComPtr<ID3D11BlendState> previousBlend_;
    std::array<float, 4> blendFactor_{};
    UINT sampleMask_{0xFFFFFFFFU};
    NativeUiCaptureKind kind_{NativeUiCaptureKind::none};
    bool active_{};
};

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

[[nodiscard]] bool IsProductionTemporalTarget(const DrawObservation& record) noexcept
{
    return g_scene.productionOperatorApplied && g_scene.currentScene != nullptr &&
        record.currentSecondaryRtv != nullptr &&
        record.currentSecondaryRtv == g_presentation.presentationProxyRtv.Get() &&
        g_presentation.presentationProxySrv != nullptr;
}

[[nodiscard]] bool IsProductionTemporal(const DrawObservation& record,
    const PipelineHeartbeatEvaluation& heartbeat) noexcept
{
    return heartbeat.currentGeneration && heartbeat.currentFrame && heartbeat.ready &&
        IsProductionTemporalTarget(record);
}

[[nodiscard]] bool IsProductionDraw(ProductionDrawKind kind, const DrawObservation& record,
    bool configurationMatches, const PipelineHeartbeatEvaluation& heartbeat) noexcept
{
    if (!configurationMatches || !g_configuration.productionRequested) return false;
    if (kind.toneMap) return IsProductionScene(record, heartbeat);
    if (kind.lut) return IsProductionScene(record, heartbeat) && heartbeat.toneMapFamilyObserved;
    if (kind.temporal)
    {
        // Unrelated TAA branches bind neutral b13; only scene/proxy lineage
        // may undo and restore production UI-relative transport around history.
        return IsProductionTemporal(record, heartbeat);
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
        production_ = IsProductionDraw(kind, record, configurationMatches, heartbeat);
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
        g_referenceHealthy = false;
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

void CloseRetainedArmorReplacement(const DrawObservation& record) noexcept
{
    if (record.currentReplacementActive)
    {
        ArmorUiScopeToken outer{};
        outer.serial = g_armor.armorEncodedScope;
        ArmorEncodedBoundary(outer, false);
        g_armor.armorEncodedFailed = true;
        g_armor.armorEncodedStatus = "Disabled: retained scene replacement inside armor scope";
    }
}

void RecordNativeUiCapture(NativeUiCaptureKind kind) noexcept
{
    if (kind == NativeUiCaptureKind::backdrop)
        g_evidence.nativeUiBackdropCaptureDrawCount.fetch_add(
            1, std::memory_order_relaxed);
    else
        g_evidence.nativeUiCaptureDrawCount.fetch_add(
            1, std::memory_order_relaxed);
}

void RecordProductionDraw(const ScopedProductionBindings& operatorBindings) noexcept
{
    if (operatorBindings.UsesProductionSettings())
        ObserveQualifiedSettingsDrawThread();
    if (operatorBindings.UsesProductionOperator())
    {
        g_scene.productionOperatorApplied = true;
        g_evidence.productionOperatorDrawCount.fetch_add(
            1, std::memory_order_relaxed);
    }
}

void ExecuteObservedDraw(
    DrawObservation& record,
    ID3D11DeviceContext* context,
    bool exactIndexedQuad,
    const DrawInvocation& draw) noexcept
{
    if (context != g_rendererDevice.activeContext)
    {
        draw();
        return;
    }
    if (g_armor.armorEncodedActive && context == g_rendererDevice.activeContext)
    {
        CloseRetainedArmorReplacement(record);
        draw();
        ObservePipboyScreenDraw(context, record);
        return;
    }
    if (!ValidateExpectedShaderBinding(context, record))
    {
        draw();
        ObservePipboyScreenDraw(context, record);
        return;
    }
    {
        ScopedGameUiCapture gameUiCapture(
            context, record, exactIndexedQuad);
        if (gameUiCapture.Active())
        {
            RecordNativeUiCapture(gameUiCapture.Kind());
            draw();
            ObservePipboyScreenDraw(context, record);
            return;
        }
    }
    {
        ScopedProductionBindings operatorBindings(context, record);
        {
            ScopedNativeReferenceDraw grading(context, record,
                operatorBindings.UsesProductionSettings());
            draw();
        }
        ObservePipboyScreenDraw(context, record);
        RecordProductionDraw(operatorBindings);
    }
    try { ObserveCompletedDraw(context, record); }
    catch (...) { RecordAbiException(); }
}


} // namespace fo4hdr
