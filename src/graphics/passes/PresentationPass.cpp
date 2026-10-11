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

#include "PresentationPass.hpp"
#include "../FrameTrace.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "../resources/PresentationResources.hpp"
#include "../resources/SceneResources.hpp"
#include "../RendererLifetime.hpp"
#include "../LiveSettings.hpp"
#include "../PresentFrame.hpp"
#include "../SharedUiFrame.hpp"
#include "../SharedSdrFrame.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LutProfileBindings.hpp"
#include "../DisplayDiscovery.hpp"
#include "../resources/ArmorCompositionResources.hpp"
#include "../resources/DeveloperExposureResources.hpp"
#include "../resources/ReplacementShaders.hpp"
#include <FO4Isotope/output/ArmorUiComposition.hpp>
#include <backends/imgui_impl_dx11.h>
#include <imgui.h>
#include "../d3d11/BorrowedComputeState.hpp"
#include "../d3d11/BorrowedFullscreenState.hpp"
#include "../hooks/ContextHooks.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

namespace {

bool PipboyObservationAvailable(const DrawObservation* active) noexcept
{
    return active && active->immediate && g_referenceHealthy &&
        g_gameplayReadyApplied.load(std::memory_order_acquire) &&
        g_evidence.hdrColorSpaceActive.load(std::memory_order_acquire);
}

void BeginPipboyObservation() noexcept
{
    auto* active = FindDrawObservation(g_rendererDevice.activeContext);
    if (PipboyObservationAvailable(active))
    {
        g_pipboyScreen = {};
        g_pipboyScreen.state.Begin(
            g_evidence.resourceGeneration.load(std::memory_order_acquire),
            active->generation);
        g_pipboyWatchedContext.store(active->generation,
            std::memory_order_release);
        g_pipboyScreen.status = "Waiting for captured shader identities";
    }
    else g_pipboyScreen.status = "Unavailable: requires healthy gameplay HDR";
}


void SettingsFocusLost() noexcept
{
    StopPipboyScreenObservation("Stopped: focus loss");
    ResetArmorUiScope();
    g_developerExposure.state.Request(false);
}
void RequestExposure(bool requested) noexcept
{
    g_developerExposure.state.Request(requested);
    g_developerExposure.failed = false;
}
void RequestPipboyObservation(bool requested) noexcept
{
    StopPipboyScreenObservation("Off");
    if (requested) BeginPipboyObservation();
}

[[nodiscard]] bool RenderDeveloperUiDrawData(ID3D11DeviceContext* context) noexcept
{
    const auto* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->TotalVtxCount == 0) return false;
    const float clear[4]{};
    context->ClearRenderTargetView(g_uiTransport.uiRtv.Get(), clear);
    ComPtr<ID3D11RenderTargetView> previousRtv;
    ComPtr<ID3D11DepthStencilView> previousDsv;
    context->OMGetRenderTargets(1, &previousRtv, &previousDsv);
    ID3D11RenderTargetView* uiRtv = g_uiTransport.uiRtv.Get();
    context->OMSetRenderTargets(1, &uiRtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ID3D11RenderTargetView* restore = previousRtv.Get();
    context->OMSetRenderTargets(1, &restore, previousDsv.Get());
    return true;
}


SettingsPipboyViewObservation UiPipboyView(const PipboyScreenViewObservation& value) noexcept
{
    return {value.frame, value.texture, value.format, value.dimension, value.mip, value.mipCount};
}
SettingsPipboyView UiPipboySnapshot() noexcept
{
    const auto& source = g_pipboyScreen;
    const auto& state = source.state;
    return {{state.requested, state.generation, state.contextGeneration, state.producerDraws, state.consumerDraws, state.pendingConsumers, state.matchedConsumers, state.unmatchedConsumers, state.rejectedDraws, state.lastProducerFrame, state.lastConsumerFrame, state.lastMatchedFrame, state.lastProducerAge},
        UiPipboyView(source.producerInput), UiPipboyView(source.producerOutput),
        UiPipboyView(source.consumerInput), source.rejections, source.producerResourceChanges, source.status};
}
SettingsDisplayView UiDisplaySnapshot() noexcept
{
    const auto& source = CurrentDisplayCapabilities();
    SettingsDisplayView result;
    result.known = source.known;
    result.monitorDevicePath = source.monitorDevicePath;
    result.friendlyName = source.friendlyName;
    std::copy(std::begin(source.description.DeviceName), std::end(source.description.DeviceName), result.deviceName.begin());
    result.peakNits = source.description.MaxLuminance;
    result.fullFrameNits = source.description.MaxFullFrameLuminance;
    result.sdrWhiteNits = source.sdrWhiteNits;
    return result;
}
WeatherLutResponseView UiWeatherLutResponseSnapshot() noexcept
{
    const bool current = g_liveSettingsThreadCompatible.load(std::memory_order_acquire) &&
        g_lutInspectorFrame == g_frame;
    if (current)
        return {g_lutInspectorLuts, g_lutInspectorValidatedSlots, g_lutInspectorStatus,
            g_lutInspectorImageSpaces, g_lutInspectorImageSpaceSlots, g_lutInspectorImageSpaceStatus,
            g_lutInspectorReferencingContexts, g_lutInspectorResolved,
            g_lutInspectorAutomaticSlots, g_lutInspectorFallbackReasons,
            g_lutInspectorGradingBypassed};
    WeatherLutResponseView view;
    view.status = NativeLutObserverMessage();
    view.imageSpaceStatus = "Waiting for a qualified ImageSpace blend.";
    return view;
}

SettingsWindowView UiSettingsSnapshot() noexcept
{
    SettingsWindowView result;
    result.activeSettings = g_activeSettings;
    result.lutPending = g_lutPending;
    result.liveSettingsError = g_liveSettingsError;
    result.developerMode = g_configuration.developerMode;
    result.hdrActive = g_evidence.hdrColorSpaceActive.load();
    result.exposureRequested = g_developerExposure.state.Requested();
    result.exposureActive = g_developerExposure.state.Active();
    result.exposureFailed = g_developerExposure.failed;
    result.threadCompatible = g_liveSettingsThreadCompatible.load();
    result.pendingAlgorithm = g_pendingLutRequest.algorithm;
    result.pendingPeakNits = g_pendingLutRequest.displayPeakNits;
    result.nativeReferenceStatus = g_nativeReferenceStatus;
    result.armorEncodedStatus = g_armor.armorEncodedStatus;
    result.rendererFailure = GraphicsObservationFailureName(static_cast<GraphicsObservationFailure>(g_evidence.failure.load()));
    result.resourceGeneration = g_evidence.resourceGeneration.load();
    result.frame = g_frame;
    result.settingsGeneration = g_settingsGeneration;
    result.desiredSettingsGeneration = g_desiredSettingsGeneration;
    result.lutRequestGeneration = g_lutRequestGeneration;
    result.display = UiDisplaySnapshot();
    result.weatherLutResponse = UiWeatherLutResponseSnapshot();
    result.pipboy = UiPipboySnapshot();
    return result;
}

void RefreshSettingsView(SettingsWindowView& view) noexcept
{
    view = UiSettingsSnapshot();
}

bool DrawDeveloperUi(ID3D11DeviceContext* context) noexcept
{
    if (!SettingsWindowBackendReady()) return false;
    auto view = UiSettingsSnapshot();
    const SettingsWindowActions actions{
        SettingsFocusLost, RequestExposure, RequestPipboyObservation, RefreshSettingsView};
    return DrawSettingsWindow(view, actions) && RenderDeveloperUiDrawData(context);
}

} // namespace

PresentationResult PresentationMapFailure(HRESULT) noexcept
{
    return PresentationResult::failed;
}

// The caller retains borrowed state until the direct backbuffer write completes.
[[nodiscard]] PresentationResult DrawPresentation(
    ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* scene,
    const CompositionConstants& compositionConstants) noexcept
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto status = context->Map(g_presentation.constants.Get(), 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(status)) return PresentationMapFailure(status);
    std::memcpy(mapped.pData, &compositionConstants, sizeof(compositionConstants));
    context->Unmap(g_presentation.constants.Get(), 0);

    // Failure must leave the real buffer untouched under its old interpretation.
    const bool hdr = compositionConstants.mode != 0U &&
        compositionConstants.mode != 4U && compositionConstants.mode != 5U;
    if (!SetColorSpace(hdr)) return PresentationResult::failed;
    ID3D11ShaderResourceView* inputs[]{
        scene, nullptr, nullptr, g_uiTransport.uiSrv.Get()};
    ID3D11RenderTargetView* output = g_presentation.presentationBackBufferRtv.Get();
    ID3D11Buffer* constants = g_presentation.constants.Get();
    context->PSSetShaderResources(0, 4, inputs);
    context->OMSetRenderTargets(1, &output, nullptr);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->VSSetShader(g_presentation.presentVertexShader.Get(), nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_presentation.presentShader.Get(), nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(g_presentation.width),
        static_cast<float>(g_presentation.height), 0, 1};
    context->RSSetState(g_presentation.presentRasterizer.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetDepthStencilState(g_presentation.presentDepth.Get(), 0);
    context->OMSetBlendState(g_presentation.presentBlend.Get(), nullptr, UINT(-1));
    context->Draw(3, 0);
    return PresentationResult::rendered;
}

std::uint32_t FrameMode(LinearColorDomain domain) noexcept
{
    if (domain == LinearColorDomain::linearBt709Relative) return 1U;
    if (SettingsCalibrationMode() != 0U) return SettingsCalibrationMode();
    return 3U;
}

std::uint32_t SourceEncoding(LinearColorDomain domain) noexcept
{
    if (domain == LinearColorDomain::vanillaSdrCodes ||
        domain == LinearColorDomain::encodedGamma22Bt709UiRelative) return 2U;
    return domain == LinearColorDomain::encodedSrgbBt709UiRelative ? 1U : 0U;
}

ID3D11ShaderResourceView* ReferenceSource(LinearColorDomain domain) noexcept
{
    return domain == LinearColorDomain::vanillaSdrCodes ?
        g_presentation.presentationProxySrv.Get() : g_scene.currentScene.Get();
}

CompositionConstants ReferenceComposition(LinearColorDomain requiredDomain,
    bool settingsOverlayPresent) noexcept
{
    const auto vanillaSdr = requiredDomain == LinearColorDomain::vanillaSdrCodes;
    const auto encoding = SourceEncoding(requiredDomain);
    CompositionConstants result{
        .width = g_presentation.width,
        .height = g_presentation.height,
        .mode = FrameMode(requiredDomain),
        .uiPaperWhiteNits = static_cast<float>(
            g_configuration.productionRequested ?
                g_activeSettings.uiPaperWhiteNits :
                kSceneReferenceWhiteNits),
        .sceneReferenceWhiteNits =
            static_cast<float>(kSceneReferenceWhiteNits),
        .displayPeakNits =
            static_cast<float>(g_activeSettings.displayPeakNits),
        .settingsOverlayPresent = static_cast<std::uint32_t>(settingsOverlayPresent),
        .sharedUiEncoded = encoding,
    };
    if (encoding != 0U)
    {
        result.uiPaperWhiteNits = vanillaSdr ? SharedSdrPaperWhite() : SharedUiPaperWhite();
    }
    return result;
}

bool CurrentSceneMatches(LinearColorDomain requiredDomain) noexcept
{
    if (requiredDomain == LinearColorDomain::vanillaSdrCodes)
        return SharedSdrFrameReady() && g_presentation.presentationProxySrv != nullptr;
    return g_scene.currentScene != nullptr && g_scene.currentSceneDomain == requiredDomain;
}

LinearColorDomain ReferenceSceneDomain() noexcept
{
    if (SharedSdrFrameReady()) return LinearColorDomain::vanillaSdrCodes;
    if (SharedUiFrameEncoded()) return LinearColorDomain::encodedSrgbBt709UiRelative;
    return (g_configuration.productionRequested || g_scene.videoSceneReady) ?
        LinearColorDomain::linearBt2020UiRelative :
        LinearColorDomain::linearBt709Relative;
}

bool PresentationContextValid(ID3D11DeviceContext* context) noexcept
{
    const auto* record = FindDrawObservation(context);
    return context && context == g_rendererDevice.activeContext && record && record->immediate;
}

ID3D11DeviceContext* PrepareGpuPerformanceSample(IDXGISwapChain* swapChain,
    ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (!PerformanceResolutionMatches(swapChain, g_configuration.measurePerformance) ||
        !EnsureGpuPerformanceQueries(device, g_configuration.measurePerformance)) return nullptr;
    PollGpuPerformanceQueries(context);
    return context;
}

[[nodiscard]] PresentationResult RenderReferenceFrame(
    IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    if (!PresentationContextValid(context)) return PresentationResult::blocked;
    ScopedPresentationContext ownedContext(context);
    if (!ownedContext.Ready()) return PresentationResult::blocked;
    const auto requiredDomain = ReferenceSceneDomain();
    if (!CurrentSceneMatches(requiredDomain)) return PresentationResult::unavailable;
    if (!EnsureResources(swapChain, device, context))
    {
        RecordProductionResourceFailure(swapChain);
        return g_referenceHealthy ? PresentationResult::unavailable : PresentationResult::failed;
    }
    auto* measurementContext = PrepareGpuPerformanceSample(swapChain, device, context);
    BorrowedFullscreenState priorState(context, g_rendererDevice.activeContext1.Get(),
        g_presentation.presentationBackBuffer.Get());
    if (!priorState.Ready()) return PresentationResult::blocked;
    ScopedGpuPerformanceSample gpuSample(measurementContext);
    const auto settingsOverlayPresent = DrawDeveloperUi(context);
    const auto compositionConstants = ReferenceComposition(requiredDomain, settingsOverlayPresent);
    return DrawPresentation(context, ReferenceSource(requiredDomain), compositionConstants);
}

bool PublicPresentationDomainAvailable(LinearColorDomain domain) noexcept
{
    constexpr std::array domains{LinearColorDomain::linearBt709Relative,
        LinearColorDomain::linearBt2020UiRelative, LinearColorDomain::encodedSrgbBt709UiRelative,
        LinearColorDomain::encodedGamma22Bt709UiRelative, LinearColorDomain::vanillaSdrCodes};
    return std::ranges::find(domains, domain) != domains.end();
}

bool PublicPresentationScalesValid(const SharedUiPresentation& presentation) noexcept
{
    return std::isfinite(presentation.white) && presentation.white > 0.0F && presentation.white <= 10000.0F &&
        std::isfinite(presentation.armorPeakUiRelative) && presentation.armorPeakUiRelative >= 0.0F;
}

bool PublicPresentationAvailable(const SharedUiPresentation& presentation) noexcept
{
    return PublicPresentationDomainAvailable(presentation.domain) &&
        PublicPresentationScalesValid(presentation) &&
        g_presentation.presentationProxy && g_presentation.presentationProxySrv;
}

struct PublicPresentationOutput
{
    const SharedUiPresentation& presentation;
    bool sdr{};
};

PresentationResult RenderPublicSharedUiOutput(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context, PublicPresentationOutput output) noexcept
{
    const auto& presentation = output.presentation;
    if (!PresentationContextValid(context)) return PresentationResult::blocked;
    ScopedPresentationContext ownedContext(context);
    if (!ownedContext.Ready()) return PresentationResult::blocked;
    if (!PublicPresentationAvailable(presentation)) return PresentationResult::unavailable;
    if (!EnsurePresentationOutputResources(swapChain, device, context, !output.sdr))
        return PresentationResult::failed;
    BorrowedFullscreenState priorState(context, g_rendererDevice.activeContext1.Get(),
        g_presentation.presentationBackBuffer.Get());
    if (!priorState.Ready()) return PresentationResult::blocked;
    auto constants = ReferenceComposition(presentation.domain, DrawDeveloperUi(context));
    if (output.sdr) constants.mode = presentation.domain == LinearColorDomain::linearBt709Relative ? 5U : 4U;
    constants.uiPaperWhiteNits = presentation.white;
    constants.sceneReferenceWhiteNits = presentation.white;
    constants.assumedContent = static_cast<std::uint32_t>(presentation.assumed);
    constants.armorPeakUiRelative = presentation.armorPeakUiRelative;
    return DrawPresentation(context, g_presentation.presentationProxySrv.Get(), constants);
}

PresentationResult RenderPublicSharedUiFrame(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context, const SharedUiPresentation& presentation) noexcept
{
    return RenderPublicSharedUiOutput(swapChain, device, context, {presentation});
}

PresentationResult RenderPublicSharedUiSdrFrame(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context, const SharedUiPresentation& presentation) noexcept
{
    return RenderPublicSharedUiOutput(swapChain, device, context, {presentation, true});
}

CompositionConstants SdrComposition(const SharedUiPresentation* presentation) noexcept
{
    const bool hdr = presentation && presentation->white > 0.0F;
    return {
        .width = g_presentation.width,
        .height = g_presentation.height,
        .mode = hdr ? 3U : 0U,
        .uiPaperWhiteNits = hdr ? presentation->white : 1.0F,
        .sceneReferenceWhiteNits = kDeveloperReferenceNits,
        .displayPeakNits = static_cast<float>(g_activeSettings.displayPeakNits),
        .sharedUiEncoded = hdr ? 2U : 0U,
        .assumedContent = static_cast<std::uint32_t>(hdr && presentation->assumed),
    };
}

bool StartupPresentationAvailable(const SharedUiPresentation* presentation) noexcept
{
    return !presentation || (PublicPresentationAvailable(*presentation) &&
        presentation->domain == LinearColorDomain::vanillaSdrCodes);
}

[[nodiscard]] PresentationResult RenderSdrPresentationProxy(
    IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context, const SharedUiPresentation* presentation) noexcept
{
    if (!PresentationContextValid(context)) return PresentationResult::blocked;
    ScopedPresentationContext ownedContext(context);
    if (!ownedContext.Ready()) return PresentationResult::blocked;
    if (g_presentation.presentationProxySrv == nullptr) return PresentationResult::unavailable;
    if (!StartupPresentationAvailable(presentation))
        return PresentationResult::unavailable;
    if (!EnsurePresentationOutputResources(swapChain, device, context, presentation != nullptr))
        return PresentationResult::failed;

    BorrowedFullscreenState priorState(context, g_rendererDevice.activeContext1.Get(),
        g_presentation.presentationBackBuffer.Get());
    if (!priorState.Ready()) return PresentationResult::blocked;
    auto constants = SdrComposition(presentation);
    constants.settingsOverlayPresent = static_cast<std::uint32_t>(DrawDeveloperUi(context));
    return DrawPresentation(context, g_presentation.presentationProxySrv.Get(), constants);
}

bool PresentationContextAvailable(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    const std::array<const void*, 3> required{swapChain, device, context};
    return std::ranges::find(required, nullptr) == required.end();
}

[[nodiscard]] bool RenderBlackPresentationFrame(
    IDXGISwapChain* swapChain,
    ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    if (!PresentationContextAvailable(swapChain, device, context))
        return false;
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> target = g_presentation.presentationBackBufferRtv;
    if (!target)
    {
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
        if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target))) return false;
    }
    BorrowedComputeState priorState(context, g_rendererDevice.activeContext1.Get(),
        backBuffer.Get());
    constexpr float black[4]{0.0F, 0.0F, 0.0F, 1.0F};
    // Zero is black under either transfer function, even if the transition fails.
    const ScopedFrameTraceOwnedCall ownedClear;
    context->ClearRenderTargetView(target.Get(), black);
    return true;
}


} // namespace fo4hdr
