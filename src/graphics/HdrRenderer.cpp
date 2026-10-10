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

#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/hooks/HookOwnership.hpp"
#include "GraphicsDiagnostics.hpp"
#include "FrameTrace.hpp"
#include "LiveSettings.hpp"
#include "hooks/DeviceHooks.hpp"
#include "passes/ArmorCompositionPass.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include <FO4Isotope/output/ArmorUiComposition.hpp>
#include <new>
#include <algorithm>
#include <array>
#include <utility>
#include "RendererLifetime.hpp"
#include "DisplayDiscovery.hpp"
#include "ShaderRegistry.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "resources/ReplacementShaders.hpp"
#include "resources/DeveloperExposureResources.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include "LutProfileBindings.hpp"
#include "DrawObservation.hpp"
#include <FO4Isotope/output/NativeLutObservation.hpp>
#include "PresentFrame.hpp"
#include "SharedUiFrame.hpp"

#include "FO4Isotope/output/Hdr10Color.hpp"
#include <FO4Isotope/tonemap/OperatorLut.hpp>
#include <FO4Isotope/output/SelectiveColor.hpp>

namespace fo4hdr {
RendererDevice g_rendererDevice;
HWND g_outputWindow{};
PipelineHeartbeat g_heartbeat;
HdrRendererLifecycle g_lifecycle;
std::uint64_t g_frame{1};
std::atomic<bool> g_referenceHealthy{true};
std::atomic<bool> g_productionHealthy{true};
std::atomic<bool> g_gameplayReadyApplied{};
std::atomic<bool> g_rendererCreationClaimed{};
[[nodiscard]] std::uint64_t PublishLifecycleGeneration() noexcept
{
    const auto generation = g_lifecycle.Snapshot().resourceGeneration;
    g_evidence.resourceGeneration.store(generation,
        std::memory_order_release);
    if (CurrentWideGamutReadiness().Complete())
    {
        g_presentation.colorDomainGeneration = generation;
        g_evidence.colorDomainGeneration.store(
            generation, std::memory_order_release);
    }
    return generation;
}


[[nodiscard]] WideGamutResourceReadiness CurrentWideGamutReadiness() noexcept
{
    return {
        .replacementShaderSet = ReplacementProgramsReady(),
        .presentationProxySet = PresentationProxyReady(),
        .compositionSet = PresentationCompositionReady(),
        .uiSet = DeveloperUiResourcesReady(),
        .outputSet = PresentationOutputReady()
    };
}

void ResetSceneTracking() noexcept
{
    ResetNativeLutObservation();
    g_lutInspectorLuts = {};
    g_lutInspectorImageSpaces = {};
    g_lutInspectorImageSpaceSlots = 0;
    g_lutInspectorFrame = 0;
    g_lutInspectorValidatedSlots = 0;
    StopPipboyScreenObservation("Stopped: scene or resource retirement");
    CloseArmorEncodedCompositionForLifecycle();
    ResetSharedUiContent();
    // Loading/focus/resource retirement ends the session; retained TLS tokens
    // cannot be accepted after the request generation changes.
    ResetArmorUiScope();
    g_developerExposure.state.Reset();
    g_developerExposure.observedFrame = 0;
    ResetSceneResources();
}
void UnbindPresentationProxy() noexcept
{
    if (g_rendererDevice.activeContext == nullptr) return;
    if (!g_presentation.presentationProxyRtv) return;
    auto* record = FindDrawObservation(g_rendererDevice.activeContext);
    if (record == nullptr || OriginalRenderTargetSetter(g_rendererDevice.activeContext) == nullptr) return;
    const bool proxyBound =
        record->currentPrimaryRtv == g_presentation.presentationProxyRtv.Get() ||
        record->currentSecondaryRtv == g_presentation.presentationProxyRtv.Get();
    if (!proxyBound) return;
    InvokeCapturedNext(OriginalRenderTargetSetter(g_rendererDevice.activeContext),
        g_rendererDevice.activeContext, 0, nullptr, nullptr);
    record->currentPrimaryRtv = nullptr;
    record->currentSecondaryRtv = nullptr;
}


void ResetResources(bool retainColorControl, bool retainPresentationBuffer) noexcept
{
    RetireFrameTrace();
    if (!retainPresentationBuffer) SetSharedUiPauseResourcesReady(false);
    g_nativeReferenceStatus = "Waiting for a color grading draw.";
    CloseArmorEncodedCompositionForLifecycle();
    // A held comparison is transient UI state. Restore its unsaved Image
    // draft before retiring the device generation so it cannot become the
    // state applied after HDR/resource recovery.
    RestoreHeldImageComparison();
    UnbindPresentationProxy();
    ResetSceneTracking();
    ResetSharedUiResources(retainPresentationBuffer);
    ResetDeveloperExposureResources();
    g_presentation.presentationRedirectedThisFrame = false;
    ResetArmorCompositionResources();
    // Retire the real output UAV before the transport images and proxy.
    RetirePresentationOutput();
    ResetUiTransportResources();
    ResetPresentationResources(retainColorControl, retainPresentationBuffer);
}

[[nodiscard]] bool EnsureResources(
    IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (PresentationResourcesReady()) {
        // Prepare the optional armor conversion outside the Draw path.
        EnsureArmorEncodedResources(device, context);
        return EnsureSharedUiResources(device);
    }
    const auto creation = CreatePresentationResources(swapChain, device, context);
    if (creation == PresentationResourceCreation::compositionFailed)
        ResetResources(false, ApplicationPresentationFacadeActive());
    if (creation != PresentationResourceCreation::ready) return false;
    EnsureArmorEncodedResources(device, context);
    EnsureSettingsWindowBackend(g_configuration.productionRequested || g_configuration.showDeveloperOverlay,
        g_outputWindow, device, context);
    if (CurrentWideGamutReadiness().Complete()) {
        g_presentation.colorDomainGeneration =
            g_evidence.resourceGeneration.load(std::memory_order_acquire);
        g_evidence.colorDomainGeneration.store(
            g_presentation.colorDomainGeneration, std::memory_order_release);
    }
    return EnsureSharedUiResources(device);
}

[[nodiscard]] bool EnsureWideGamutResources(
    IDXGISwapChain* swapChain,
    ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    return CreatePresentationProxy(swapChain, device) &&
        EnsureResources(swapChain, device, context) &&
        CurrentWideGamutReadiness().Complete();
}

bool DisplayRebuildFailed(IDXGISwapChain* swapChain, int hdrState) noexcept
{
    return hdrState == 1 && HdrPathRequested() && g_rendererDevice.activeDevice != nullptr &&
        !EnsureWideGamutResources(swapChain, g_rendererDevice.activeDevice, g_rendererDevice.activeContext);
}

void RecordProductionResourceFailure(IDXGISwapChain* swapChain) noexcept
{
    g_productionHealthy = false;
    if (!EnsurePresentationOutputResources(swapChain, g_rendererDevice.activeDevice,
            g_rendererDevice.activeContext, true))
        g_referenceHealthy = false;
    RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
}

void RebuildChangedDisplay(IDXGISwapChain* swapChain, int hdrState) noexcept
{
    if (!DisplayRebuildFailed(swapChain, hdrState)) return;
    g_referenceHealthy = false;
    RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
}

void BeginPublishedHeartbeatGeneration() noexcept
{
    const auto generation = PublishLifecycleGeneration();
    g_frame = 1;
    g_heartbeat.Reset(generation);
    g_heartbeat.BeginFrame(generation, g_frame);
}

struct DisplayGenerationObservation
{
    DXGI_SWAP_CHAIN_DESC description{};
    HMONITOR monitor{};
    int hdrState{};
    int previousHdrState{};

    void Read(IDXGISwapChain* swapChain) noexcept
    {
        const auto outputKnown = swapChain != nullptr &&
            SUCCEEDED(swapChain->GetDesc(&description)) &&
            RefreshDisplayCapabilities(description.OutputWindow);
        if (!outputKnown) InvalidateDisplayCapabilities();
        monitor = outputKnown ? CurrentDisplayCapabilities().description.Monitor : nullptr;
        hdrState = outputKnown && CurrentDisplayCapabilities().description.ColorSpace ==
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 1 : 0;
        previousHdrState = ActiveDisplayHdrState();
    }

    bool FirstObservation() const noexcept
    {
        return ActiveDisplayMonitor() == nullptr && previousHdrState < 0;
    }

    bool Changed() const noexcept
    {
        return ActiveDisplayMonitor() != monitor || previousHdrState != hdrState;
    }

    void PublishMonitor() const noexcept
    {
        PublishActiveDisplay(monitor, hdrState);
    }

    void ApplyChange(IDXGISwapChain* swapChain, bool& sdrColorSpaceEstablished) const noexcept
    {
        if (g_presentation.swapChain3) sdrColorSpaceEstablished = SetColorSpace(false);
        PublishMonitor();
        g_lifecycle.OnDisplayChange();
        ResetResources();
        g_presentation.width = description.BufferDesc.Width;
        g_presentation.height = description.BufferDesc.Height;
        g_referenceHealthy = true;
        RebuildChangedDisplay(swapChain, hdrState);
        BeginPublishedHeartbeatGeneration();
    }
};

[[nodiscard]] bool DisplayGenerationChanged(IDXGISwapChain* swapChain,
    bool& sdrColorSpaceEstablished) noexcept
{
    sdrColorSpaceEstablished = false;
    DisplayGenerationObservation observation;
    observation.Read(swapChain);
    if (observation.FirstObservation())
    {
        observation.PublishMonitor();
        return false;
    }
    if (!observation.Changed()) return false;
    observation.ApplyChange(swapChain, sdrColorSpaceEstablished);
    return true;
}
void RetireLostDeviceGeneration() noexcept
{
    ShutdownSettingsWindowBackend();
    ResetResources();
    ResetReplacementShaders();
    ResetShaderRegistry();
    g_lifecycle.OnDeviceLost();
    (void)PublishLifecycleGeneration();
    g_rendererDevice.activeDevice = nullptr;
    g_rendererDevice.activeContext = nullptr;
    g_rendererDevice.activeContext1.Reset();
    g_outputWindow = nullptr;
    ResetDisplayDiscovery();
    // The claim is scoped to the retired device generation. A replacement
    // D3D creation may make one fresh bounded HDR attempt.
    g_rendererCreationClaimed.store(false, std::memory_order_release);
    g_referenceHealthy.store(true, std::memory_order_release);
}



PluginConfiguration g_configuration{};
HdrSettings g_activeSettings{};
std::uint64_t g_settingsGeneration{1};
std::uint64_t g_desiredSettingsGeneration{1};
std::uint64_t g_lutRequestGeneration{};
std::uint64_t g_deviceGeneration{};
bool g_lutPending{};
OperatorLutBuildRequest g_pendingLutRequest{};
namespace {
bool g_lutSubmissionNeeded{};
unsigned g_lutSubmissionAttempts{};
}
std::unique_ptr<OperatorLutBuildResult> g_activeCpuLut;
bool g_liveSettingsError{};
std::atomic<DWORD> g_currentFrameQualifiedDrawThread{};
std::atomic<bool> g_currentFrameQualifiedDrawThreadConflict{};
std::atomic<DWORD> g_observedQualifiedDrawThread{};
std::atomic<DWORD> g_observedPresentThread{};
std::atomic<bool> g_liveSettingsThreadCompatible{true};

namespace {

[[nodiscard]] std::array<float, 4> OperatorColorVector(
    double x, double y, double z) noexcept
{
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 0.0F};
}

[[nodiscard]] bool SetProductionOperatorMode(
    ProductionToneMapperAlgorithm algorithm, OperatorConstants& settings) noexcept
{
    switch (algorithm)
    {
    case ProductionToneMapperAlgorithm::aces2: settings.mode = 1U; return true;
    case ProductionToneMapperAlgorithm::agX: settings.mode = 2U; return true;
    case ProductionToneMapperAlgorithm::gt7: settings.mode = 3U; return true;
    default: return false;
    }
}

} // namespace

[[nodiscard]] bool BuildOperatorSettings(
    const HdrSettings& values,
    OperatorConstants& settings) noexcept
{
    const auto math = DeriveHdrSettingsMath(values);
    SelectiveColorPrepared selective{};
    if (!PrepareSelectiveColor(values.selectiveColor, selective))
        return false;
    const auto shaper = OperatorLutShaperParameters(
        values.productionAlgorithm, values.displayPeakNits);
    settings = {
        .mode = 0U,
        .displayPeakNits =
            static_cast<float>(values.displayPeakNits),
        .sceneReferenceWhiteNits =
            static_cast<float>(kSceneReferenceWhiteNits),
        .uiTransportWhiteNits =
            static_cast<float>(kSceneReferenceWhiteNits),
        .sceneExposureMultiplier = 1.0F,
        .videoWhiteNits = kVideoWhiteNits,
        .operatorShaperAngleGain = shaper[0],
        .operatorShaperBias = shaper[1],
        .bloomStrength = static_cast<float>(math.bloomMultiplier),
        .vanillaColorGradingStrength =
            static_cast<float>(math.vanillaColorGradingBlend),
        .contrastSlope = static_cast<float>(math.contrastSlope),
        .highlightsEv = static_cast<float>(math.highlightsEv),
        .shadowsEv = static_cast<float>(math.shadowsEv),
        .saturationScale = static_cast<float>(math.saturationScale),
        .liveSettingsReserved = 0U,
        .blacksStrength = static_cast<float>(math.blacksStrength),
        .colorBalanceRow0 = OperatorColorVector(math.colorBalanceBt709[0],
            math.colorBalanceBt709[1], math.colorBalanceBt709[2]),
        .colorBalanceRow1 = OperatorColorVector(math.colorBalanceBt709[3],
            math.colorBalanceBt709[4], math.colorBalanceBt709[5]),
        .colorBalanceRow2 = OperatorColorVector(math.colorBalanceBt709[6],
            math.colorBalanceBt709[7], math.colorBalanceBt709[8]),
        .selectiveColorActive = selective.effectiveActive ? 1U : 0U,
        .liveSettingsReserved1 = 0U,
        .operatorShaperParameters = {shaper[2], shaper[3]},
        .selectiveColorPlaneA = OperatorColorVector(
            selective.planeA[0], selective.planeA[1], selective.planeA[2]),
        .selectiveColorPlaneB = OperatorColorVector(
            selective.planeB[0], selective.planeB[1], selective.planeB[2])
    };
    for (std::size_t index = 0; index < kSelectiveColorRegionCount; ++index)
        settings.selectiveColorRegions[index] = OperatorColorVector(
            selective.regions[index][0], selective.regions[index][1], selective.regions[index][2]);
    settings.selectiveColorPlaneA[3] =
        static_cast<float>(math.vanillaGradeLuminanceRemoval);
    settings.selectiveColorPlaneB[3] =
        static_cast<float>(math.vanillaGradeColorRemoval);
    if (!g_configuration.productionRequested) return true;

    if (!SetProductionOperatorMode(values.productionAlgorithm, settings)) return false;
    settings.uiTransportWhiteNits =
        static_cast<float>(values.uiPaperWhiteNits);
    settings.sceneExposureMultiplier =
        static_cast<float>(math.sceneExposureMultiplier);
    settings.videoWhiteNits = static_cast<float>(values.videoWhiteNits);
    return true;
}

[[nodiscard]] bool BuildOperatorSettings(
    OperatorConstants& settings) noexcept
{
    return BuildOperatorSettings(g_activeSettings, settings);
}

[[nodiscard]] bool RequiresOperatorLut(std::uint32_t mode) noexcept
{
    return mode >= 1U && mode <= 3U;
}


void ObserveQualifiedSettingsDrawThread() noexcept
{
    const auto thread = GetCurrentThreadId();
    DWORD empty{};
    if (!g_currentFrameQualifiedDrawThread.compare_exchange_strong(
            empty, thread, std::memory_order_acq_rel) && empty != thread)
    {
        g_currentFrameQualifiedDrawThreadConflict.store(
            true, std::memory_order_release);
    }
    g_observedQualifiedDrawThread.store(thread, std::memory_order_release);
}

[[nodiscard]] bool HdrPathRequested() noexcept
{
    return g_configuration.enabled &&
        (g_configuration.productionRequested ||
         g_configuration.developerHdr10Reference);
}


void InitializeLiveSettings(const PluginConfiguration& configuration) noexcept
{
    g_activeSettings = static_cast<const HdrSettings&>(configuration);
    InitializeSettingsWindow(g_activeSettings, configuration.interfaceSettings);
    g_settingsGeneration = 1;
    g_desiredSettingsGeneration = 1;
    g_lutRequestGeneration = 0;
    g_lutPending = false;
    g_pendingLutRequest = {};
    g_lutSubmissionNeeded = false;
    g_lutSubmissionAttempts = 0;
    g_liveSettingsError = false;
}

[[nodiscard]] bool SameOperatorKey(
    const HdrSettings& left,
    const HdrSettings& right) noexcept
{
    return left.productionAlgorithm == right.productionAlgorithm &&
        left.displayPeakNits == right.displayPeakNits &&
        ActiveToneMapperParameters(left.productionAlgorithm,
            left.toneMapperParameters) ==
            ActiveToneMapperParameters(right.productionAlgorithm,
                right.toneMapperParameters);
}

void ApplyOperatorKey(
    ProductionToneMapperAlgorithm algorithm,
    double displayPeakNits,
    const ToneMapperParameters& parameters,
    HdrSettings& settings) noexcept
{
    settings.productionAlgorithm = algorithm;
    settings.displayPeakNits = displayPeakNits;
    switch (algorithm)
    {
    case ProductionToneMapperAlgorithm::aces2:
        settings.toneMapperParameters.acesLookStrength =
            parameters.acesLookStrength;
        break;
    case ProductionToneMapperAlgorithm::agX:
        settings.toneMapperParameters.agxMiddleGrayEv =
            parameters.agxMiddleGrayEv;
        settings.toneMapperParameters.agxContrast = parameters.agxContrast;
        settings.toneMapperParameters.agxHdrPurity = parameters.agxHdrPurity;
        settings.toneMapperParameters.agxHdrShoulderPower =
            parameters.agxHdrShoulderPower;
        break;
    case ProductionToneMapperAlgorithm::gt7:
        settings.toneMapperParameters.gt7ToeStrength =
            parameters.gt7ToeStrength;
        settings.toneMapperParameters.gt7ShoulderStart =
            parameters.gt7ShoulderStart;
        settings.toneMapperParameters.gt7ColorBlend =
            parameters.gt7ColorBlend;
        break;
    }
}

void ApplyOperatorKey(
    const HdrSettings& source,
    HdrSettings& destination) noexcept
{
    ApplyOperatorKey(source.productionAlgorithm, source.displayPeakNits,
        source.toneMapperParameters, destination);
}

void ApplyOperatorKey(
    const OperatorLutBuildRequest& source,
    HdrSettings& destination) noexcept
{
    ApplyOperatorKey(source.algorithm, source.displayPeakNits,
        source.toneMapperParameters, destination);
}

[[nodiscard]] OperatorLutBuildRequest DesiredOperatorLutBuild() noexcept
{
    return {
        .algorithm = ReadSettingsDraft().productionAlgorithm,
        .displayPeakNits = ReadSettingsDraft().displayPeakNits,
        .deviceGeneration = g_deviceGeneration,
        .settingsGeneration = g_desiredSettingsGeneration,
        .requestGeneration = g_lutRequestGeneration,
        .toneMapperParameters = ActiveToneMapperParameters(
            ReadSettingsDraft().productionAlgorithm,
            ReadSettingsDraft().toneMapperParameters)
    };
}

[[nodiscard]] OperatorLutBuildRequest PendingOperatorLutBuild() noexcept
{
    auto request = g_pendingLutRequest;
    request.deviceGeneration = g_deviceGeneration;
    request.settingsGeneration = g_desiredSettingsGeneration;
    return request;
}

[[nodiscard]] bool PendingOperatorKeyMatches(
    const HdrSettings& settings) noexcept
{
    return g_lutPending &&
        g_pendingLutRequest.algorithm == settings.productionAlgorithm &&
        g_pendingLutRequest.displayPeakNits == settings.displayPeakNits &&
        g_pendingLutRequest.toneMapperParameters ==
            ActiveToneMapperParameters(settings.productionAlgorithm,
                settings.toneMapperParameters);
}

void RetirePendingOperatorLutBuild() noexcept
{
    if (g_lutPending)
        CancelOperatorLutBuild(++g_lutRequestGeneration);
    g_lutPending = false;
    g_lutSubmissionNeeded = false;
    g_pendingLutRequest = {};
}

void SubmitPendingOperatorLutBuild() noexcept
{
    const auto status = RequestOperatorLutBuild(g_pendingLutRequest);
    g_lutSubmissionNeeded = RetryOperatorLutSubmission(
        status, g_lutSubmissionAttempts);
    if (status != OperatorLutRequestStatus::accepted && !g_lutSubmissionNeeded)
    {
        RetirePendingOperatorLutBuild();
        g_liveSettingsError = true;
    }
}

void QueueOperatorLutBuild(OperatorLutBuildRequest request) noexcept
{
    ++g_lutRequestGeneration;
    request.deviceGeneration = g_deviceGeneration;
    request.settingsGeneration = g_desiredSettingsGeneration;
    request.requestGeneration = g_lutRequestGeneration;
    request.toneMapperParameters = ActiveToneMapperParameters(
        request.algorithm, request.toneMapperParameters);
    g_lutPending = true;
    g_pendingLutRequest = request;
    g_lutSubmissionAttempts = 0;
    SubmitPendingOperatorLutBuild();
}

void QueueDesiredOperatorLutBuild() noexcept
{
    QueueOperatorLutBuild(DesiredOperatorLutBuild());
}

void PublishImmediateSettings(ID3D11DeviceContext* context,
    const HdrSettings& immediate) noexcept
{
    if (!PublishReplacementSettings(context, immediate, g_desiredSettingsGeneration))
    {
        g_liveSettingsError = true;
        return;
    }
    g_activeSettings = immediate;
    g_settingsGeneration = g_desiredSettingsGeneration;
}

void UpdateDesiredOperator(bool operatorChanged,
    const HdrSettings& desired) noexcept
{
    if (!operatorChanged)
    {
        RetirePendingOperatorLutBuild();
        return;
    }
    if (!PendingOperatorKeyMatches(desired))
        QueueDesiredOperatorLutBuild();
}

void CommitUiDraft(ID3D11DeviceContext* context) noexcept
{
    (void)ConsumeSettingsCommitRequest();
    g_liveSettingsError = false;
    if (ValidateHdrSettings(ReadSettingsDraft()) != HdrSettingsValidationError::none)
    {
        g_liveSettingsError = true;
        return;
    }
    ++g_desiredSettingsGeneration;
    auto desired = ReadSettingsDraft();
    const auto operatorChanged = !SameOperatorKey(g_activeSettings, desired);
    auto immediate = desired;
    if (operatorChanged)
        ApplyOperatorKey(g_activeSettings, immediate);
    PublishImmediateSettings(context, immediate);
    UpdateDesiredOperator(operatorChanged, desired);
}

void FailPendingOperatorLut() noexcept
{
    g_lutPending = false;
    g_pendingLutRequest = {};
    g_liveSettingsError = true;
}

void PublishCompletedOperatorLut(ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::unique_ptr<OperatorLutBuildResult> result) noexcept
{
    auto completedSettings = g_activeSettings;
    ApplyOperatorKey(result->request, completedSettings);
    if (!PublishReplacementLut(device, context, *result, completedSettings))
    {
        FailPendingOperatorLut();
        return;
    }
    g_activeCpuLut = std::move(result);
    g_activeSettings = completedSettings;
    g_lutPending = false;
    g_pendingLutRequest = {};
    g_liveSettingsError = g_settingsGeneration != g_desiredSettingsGeneration;
}

void CommitCompletedOperatorLut(ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    auto result = TakeCompletedOperatorLutBuild();
    const auto failedRequestGeneration = TakeFailedOperatorLutBuildGeneration();
    if (g_lutPending && failedRequestGeneration == g_lutRequestGeneration)
        FailPendingOperatorLut();
    const auto completionAction = result ?
        ClassifyOperatorLutCompletion(
            result->request, PendingOperatorLutBuild(), g_lutPending) :
        OperatorLutCompletionAction::retire;
    if (completionAction != OperatorLutCompletionAction::publish) return;
    if (!PreparedOperatorLutMatches(result->data, result->request))
    {
        FailPendingOperatorLut();
        return;
    }
    PublishCompletedOperatorLut(device, context, std::move(result));
}

bool CanCommitLiveSettings(ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    return g_configuration.productionRequested && device != nullptr &&
        context != nullptr;
}

void CommitLiveSettingsAtEndPresent(ID3D11Device* device,
    ID3D11DeviceContext* context) noexcept
{
    if (!CanCommitLiveSettings(device, context)) return;
    if (!g_liveSettingsThreadCompatible.load(std::memory_order_acquire))
    {
        if (SettingsCommitRequested() || g_lutPending)
            g_liveSettingsError = true;
        return;
    }
    if (g_lutSubmissionNeeded) SubmitPendingOperatorLutBuild();
    if (SettingsCommitRequested()) CommitUiDraft(context);
    CommitCompletedOperatorLut(device, context);
    RefreshWeatherGradeResponses();
}


namespace {

OneShotInitialization g_initialization;

[[nodiscard]] bool GraphicsDiagnosticsRequested(const PluginConfiguration& configuration) noexcept
{
    return configuration.developerMode &&
        (configuration.traceRenderPipeline || configuration.measurePerformance);
}

[[nodiscard]] bool GraphicsHdrRequested(const PluginConfiguration& configuration) noexcept
{
    return configuration.enabled &&
        (configuration.productionRequested ||
            (configuration.developerMode && configuration.developerHdr10Reference));
}

void PublishGraphicsRequest(const PluginConfiguration& configuration, bool hdrRequested) noexcept
{
    g_evidence.performanceRequested.store(configuration.measurePerformance, std::memory_order_release);
    g_evidence.performanceActivePath.store(hdrRequested, std::memory_order_release);
    g_evidence.requested.store(true, std::memory_order_release);
    g_evidence.referenceRequested.store(
        configuration.enabled && configuration.developerHdr10Reference, std::memory_order_release);
    g_evidence.productionRequested.store(
        configuration.enabled && configuration.productionRequested, std::memory_order_release);
}

void InitializeGraphicsSettings(const PluginConfiguration& configuration) noexcept
{
    g_configuration = configuration;
    SetArmorUiBoundaryCallback([](const ArmorUiScopeToken& scope, bool entering) noexcept {
        TraceNativeBoundary(scope, entering);
        if (entering) SharedUiBoundary(scope, true);
        ArmorEncodedBoundary(scope, entering);
        if (!entering) SharedUiBoundary(scope, false);
    });
    SetArmorUiObservationEnabled(InitializeFrameTrace(
        configuration.developerMode && configuration.traceRenderPipeline));
    SetArmorUiCorrectionEnabled(configuration.enabled && configuration.productionRequested);
    InitializeLiveSettings(configuration);
}

[[nodiscard]] bool PrepareInitialOperatorLut(const PluginConfiguration& configuration) noexcept
{
    if (!configuration.enabled || !configuration.productionRequested) return true;
    // Prepare before intercepting device creation, which requires a complete
    // operator. Retain these device-independent bytes for later recreation.
    g_activeCpuLut.reset(new (std::nothrow) OperatorLutBuildResult{});
    if (g_activeCpuLut) {
        g_activeCpuLut->request = DesiredOperatorLutBuild();
        g_activeCpuLut->data = BuildOperatorLut(
            g_activeSettings.productionAlgorithm, g_activeSettings.displayPeakNits,
            ActiveToneMapperParameters(g_activeSettings.productionAlgorithm,
                g_activeSettings.toneMapperParameters));
    }
    return g_activeCpuLut && g_activeCpuLut->data.IsValid() && InitializeOperatorLutWorker();
}

[[nodiscard]] bool FailGraphicsInitialization(GraphicsObservationFailure failure) noexcept
{
    RecordFailure(failure);
    g_initialization.Complete(false);
    return false;
}

}  // namespace

bool InitializeGraphicsObservation(const PluginConfiguration& configuration) noexcept
{
    const auto diagnosticsRequested = GraphicsDiagnosticsRequested(configuration);
    const auto hdrRequested = GraphicsHdrRequested(configuration);
    if (!hdrRequested && !diagnosticsRequested) return false;
    switch (g_initialization.Begin()) {
    case OneShotInitializationAction::alreadyActive:
        return g_configuration == configuration;
    case OneShotInitializationAction::unavailable:
        return false;
    case OneShotInitializationAction::install:
        break;
    }
    PublishGraphicsRequest(configuration, hdrRequested);
    InitializeGraphicsSettings(configuration);
    if (!PrepareInitialOperatorLut(configuration)) {
        g_activeCpuLut.reset();
        return FailGraphicsInitialization(GraphicsObservationFailure::resourceCreationFailed);
    }
    ResetPerformanceMeasurement();
    const auto failure = InstallGraphicsImport();
    if (failure != GraphicsObservationFailure::none) return FailGraphicsInitialization(failure);
    RecordFailure(GraphicsObservationFailure::none);
    g_evidence.initialized.store(true, std::memory_order_release);
    g_initialization.Complete(true);
    return true;
}

GraphicsSettingsInputActivation TryActivateGraphicsSettingsInputOnCurrentThread() noexcept
{
    const auto input = TryActivateSettingsWindowInput();
    return {input.ready, input.active, input.currentThreadId, input.windowThreadId};
}

} // namespace fo4hdr
