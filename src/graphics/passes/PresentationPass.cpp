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
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "../resources/PresentationResources.hpp"
#include "../resources/SceneResources.hpp"
#include "../RendererLifetime.hpp"
#include "../LiveSettings.hpp"
#include "../PresentFrame.hpp"
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
LutProfilesView UiLutProfilesSnapshot() noexcept
{
    const bool current = g_liveSettingsThreadCompatible.load(std::memory_order_acquire) &&
        g_lutInspectorFrame == g_frame;
    if (current)
        return {g_lutInspectorLuts, g_lutInspectorValidatedSlots, g_lutInspectorStatus,
            g_lutInspectorImageSpaces, g_lutInspectorImageSpaceSlots, g_lutInspectorImageSpaceStatus};
    LutProfilesView view;
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
    result.lutProfiles = UiLutProfilesSnapshot();
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

// The caller retains borrowed state until the direct backbuffer write completes.
[[nodiscard]] bool DrawPresentation(
    ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* scene,
    const CompositionConstants& compositionConstants) noexcept
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_presentation.constants.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &compositionConstants, sizeof(compositionConstants));
    context->Unmap(g_presentation.constants.Get(), 0);

    // Failure must leave the real buffer untouched under its old interpretation.
    if (!SetColorSpace(compositionConstants.mode != 0U)) return false;
    ID3D11ShaderResourceView* inputs[]{
        scene, g_uiTransport.gameUiBackdropSrv.Get(),
        g_uiTransport.gameUiSrv.Get(), g_uiTransport.uiSrv.Get()};
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
    return true;
}

CompositionConstants ReferenceComposition(LinearColorDomain requiredDomain,
    bool settingsOverlayPresent) noexcept
{
    const auto wideGamutScene =
        requiredDomain == LinearColorDomain::linearBt2020UiRelative;
    return {
        .width = g_presentation.width,
        .height = g_presentation.height,
        .mode = wideGamutScene && SettingsCalibrationMode() != 0U ?
            SettingsCalibrationMode() : (wideGamutScene ? 3U : 1U),
        .uiPaperWhiteNits = static_cast<float>(
            g_configuration.productionRequested ?
                g_activeSettings.uiPaperWhiteNits :
                kSceneReferenceWhiteNits),
        .sceneReferenceWhiteNits =
            static_cast<float>(kSceneReferenceWhiteNits),
        .displayPeakNits =
            static_cast<float>(g_activeSettings.displayPeakNits),
        .gameUiBackdropCaptured =
            static_cast<std::uint32_t>(g_uiTransport.gameUiBackdropCapturedThisFrame),
        .gameUiCaptured = static_cast<std::uint32_t>(g_uiTransport.gameUiCapturedThisFrame),
        .settingsOverlayPresent = static_cast<std::uint32_t>(settingsOverlayPresent),
    };
}

bool CurrentSceneMatches(LinearColorDomain requiredDomain) noexcept
{
    return g_scene.currentScene != nullptr && g_scene.currentSceneDomain == requiredDomain;
}

ID3D11DeviceContext* PrepareGpuPerformanceSample(IDXGISwapChain* swapChain,
    ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (!PerformanceResolutionMatches(swapChain, g_configuration.measurePerformance) ||
        !EnsureGpuPerformanceQueries(device, g_configuration.measurePerformance)) return nullptr;
    PollGpuPerformanceQueries(context);
    return context;
}

[[nodiscard]] bool RenderReferenceFrame(
    IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    const auto videoReady = g_scene.videoSceneReady;
    const auto requiredDomain =
        (g_configuration.productionRequested || videoReady) ?
            LinearColorDomain::linearBt2020UiRelative :
            LinearColorDomain::linearBt709Relative;
    if (!EnsureResources(swapChain, device, context) ||
        !CurrentSceneMatches(requiredDomain))
        return false;
    ScopedPresentationContext ownedContext(context);
    if (!ownedContext.Ready()) return false;
    auto* measurementContext = PrepareGpuPerformanceSample(swapChain, device, context);
    BorrowedFullscreenState priorState(context, g_rendererDevice.activeContext1.Get(),
        g_presentation.presentationBackBuffer.Get());
    if (!priorState.Ready()) return false;
    ScopedGpuPerformanceSample gpuSample(measurementContext);
    const auto settingsOverlayPresent = DrawDeveloperUi(context);
    const auto compositionConstants = ReferenceComposition(requiredDomain, settingsOverlayPresent);
    return DrawPresentation(context, g_scene.currentScene.Get(),
        compositionConstants);
}

[[nodiscard]] bool RenderSdrPresentationProxy(
    IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    if (g_presentation.presentationProxySrv == nullptr ||
        !EnsureResources(swapChain, device, context))
        return false;

    ScopedPresentationContext ownedContext(context);
    if (!ownedContext.Ready()) return false;
    BorrowedFullscreenState priorState(context, g_rendererDevice.activeContext1.Get(),
        g_presentation.presentationBackBuffer.Get());
    if (!priorState.Ready()) return false;

    const CompositionConstants compositionConstants{
        .width = g_presentation.width,
        .height = g_presentation.height,
        .mode = 0U,
        .uiPaperWhiteNits = 1.0F,
        .sceneReferenceWhiteNits = kDeveloperReferenceNits,
        .displayPeakNits = 1000.0F,
        .gameUiBackdropCaptured =
            static_cast<std::uint32_t>(g_uiTransport.gameUiBackdropCapturedThisFrame),
        .gameUiCaptured = static_cast<std::uint32_t>(g_uiTransport.gameUiCapturedThisFrame),
    };
    return DrawPresentation(context, g_presentation.presentationProxySrv.Get(),
        compositionConstants);
}

bool PresentationContextAvailable(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    const std::array<const void*, 3> required{swapChain, device, context};
    return std::ranges::find(required, nullptr) == required.end();
}

[[nodiscard]] bool RenderBlackSdrFailureFrame(
    IDXGISwapChain* swapChain,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    bool sdrColorSpaceEstablished) noexcept
{
    if (!PresentationContextAvailable(swapChain, device, context))
        return false;
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(device->CreateRenderTargetView(
            backBuffer.Get(), nullptr, &target)))
        return false;
    BorrowedComputeState priorState(context, g_rendererDevice.activeContext1.Get(),
        backBuffer.Get());
    constexpr float black[4]{0.0F, 0.0F, 0.0F, 1.0F};
    // Zero is black under either transfer function, even if the transition fails.
    context->ClearRenderTargetView(target.Get(), black);
    return sdrColorSpaceEstablished || SetColorSpace(false);
}


} // namespace fo4hdr
