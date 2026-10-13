// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "PresentFrame.hpp"
#include "SharedUiFrame.hpp"
#include "SharedSdrFrame.hpp"
#include "RendererLifetime.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "OperatorConstants.hpp"
#include "LutProfileBindings.hpp"
#include "passes/PresentationPass.hpp"
#include "passes/ArmorCompositionPass.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "resources/ReplacementShaders.hpp"
#include "resources/DeveloperExposureResources.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include "resources/HdrResourcePolicy.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"
#include <FO4Isotope/output/HdrActivationPolicy.hpp>
#include <FO4Isotope/ui/HdrSettingsInput.hpp>
#include "FO4Isotope/output/PerformanceStatistics.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;
struct GpuPerformanceQuerySlot
{
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> start;
    ComPtr<ID3D11Query> end;
    bool pending{};
};

namespace {
constexpr std::size_t kGpuPerformanceQuerySlots = 8;
struct PerformanceMeasurementState
{
    FixedPerformanceStatistics cpu;
    FixedPerformanceStatistics gpu;
    std::array<GpuPerformanceQuerySlot, kGpuPerformanceQuerySlots> gpuQueries{};
    std::size_t gpuQueryCursor{};
    bool gpuQueriesAvailable{};
};
PerformanceMeasurementState g_performance;
bool g_gameplayResourceCheckPending{};
struct OutputRecovery
{
    bool used{}, pending{};
};
OutputRecovery g_outputRecovery;

bool ApplyGraphicsLifecycle(const GraphicsLifecyclePublication& publication) noexcept
{
    const bool ready = publication.gameplayReady && !publication.presentationBlocked;
    const bool reset = g_lifecycle.OnGameReset(publication.resetEpoch);
    const bool transition = reset || ready != g_gameplayReadyApplied.load(std::memory_order_acquire);
    g_evidence.gameplayReady.store(publication.gameplayReady, std::memory_order_release);
    if (!transition) return false;

    ResetSceneTracking();
    if (reset)
    {
        g_referenceHealthy = true;
        g_productionHealthy = true;
        g_outputRecovery = {};
        (void)PublishLifecycleGeneration();
        g_evidence.gameResetEpoch.store(publication.resetEpoch, std::memory_order_release);
        g_frame = 1;
    }
    if (!ready && g_lifecycle.Snapshot().state == HdrRendererState::hdr10Ready)
        (void)g_lifecycle.PauseHdrPresentation();
    g_gameplayReadyApplied.store(ready, std::memory_order_release);
    g_gameplayResourceCheckPending = ready;
    const auto generation = g_evidence.resourceGeneration.load(std::memory_order_acquire);
    g_heartbeat.Reset(generation);
    g_heartbeat.BeginFrame(generation, g_frame);
    return true;
}
}  // namespace

void ApplyGraphicsLifecycleBeforeDraw(ID3D11DeviceContext* context) noexcept
{
    const auto* record = FindDrawObservation(context);
    const bool immediate = record && record->immediate;
    if (context != g_rendererDevice.activeContext || !immediate) return;
    // Later publications belong to Present's rejection of the already-started frame.
    if (SharedSdrFrameHasNativeWriter(context)) return;
    (void)ApplyGraphicsLifecycle(ReadGraphicsLifecycle());
}

[[nodiscard]] std::uint64_t MillisecondsToNanoseconds(
    double milliseconds) noexcept
{
    const auto nonnegative = milliseconds < 0.0 ? 0.0 : milliseconds;
    return static_cast<std::uint64_t>(
        std::llround(nonnegative * 1'000'000.0));
}

struct PerformanceCompletionEvidence
{
    std::atomic<bool>& complete;
    std::atomic<bool>& budgetPassed;
    std::atomic<std::uint64_t>& p95;
    std::atomic<std::uint64_t>& p99;
};

enum class PerformanceSampleSource
{
    cpu,
    gpu
};

PerformanceCompletionEvidence PerformanceCompletionCounters(PerformanceSampleSource source) noexcept
{
    if (source == PerformanceSampleSource::gpu)
        return {g_evidence.performanceGpuComplete, g_evidence.performanceGpuBudgetPassed,
            g_evidence.performanceGpuP95Nanoseconds, g_evidence.performanceGpuP99Nanoseconds};
    return {g_evidence.performanceCpuComplete, g_evidence.performanceCpuBudgetPassed,
        g_evidence.performanceCpuP95Nanoseconds, g_evidence.performanceCpuP99Nanoseconds};
}
void PublishPerformanceResult(
    PerformanceSampleSource source,
    PerformanceSampleResult result,
    const FixedPerformanceStatistics& statistics) noexcept
{
    auto& measured = source == PerformanceSampleSource::gpu ?
        g_evidence.performanceGpuMeasuredSamples :
        g_evidence.performanceCpuMeasuredSamples;
    measured.store(statistics.MeasuredSamples(), std::memory_order_release);
    if (result != PerformanceSampleResult::completed) return;

    const auto percentiles = statistics.Percentiles();
    auto completion = PerformanceCompletionCounters(source);
    const auto path = source == PerformanceSampleSource::gpu ? FrozenM6PerformancePath::activeGpu :
        (g_evidence.performanceActivePath.load(std::memory_order_acquire) ?
            FrozenM6PerformancePath::activeCpu :
            FrozenM6PerformancePath::disabledBoundaryCpu);
    completion.p95.store(MillisecondsToNanoseconds(percentiles.p95Milliseconds),
        std::memory_order_release);
    completion.p99.store(MillisecondsToNanoseconds(percentiles.p99Milliseconds),
        std::memory_order_release);
    completion.budgetPassed.store(
        PassesFrozenM6PerformanceBudget(path, percentiles),
        std::memory_order_release);
    completion.complete.store(true, std::memory_order_release);
}

bool UsesWindowResolution(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    return (description.BufferDesc.Width == 0 || description.BufferDesc.Height == 0) &&
        description.OutputWindow != nullptr;
}

[[nodiscard]] bool PerformanceResolutionMatches(
    IDXGISwapChain* swapChain, bool measurePerformance) noexcept
{
    if (!measurePerformance || swapChain == nullptr)
        return false;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description))) return false;
    auto width = description.BufferDesc.Width;
    auto height = description.BufferDesc.Height;
    if (UsesWindowResolution(description))
    {
        RECT client{};
        if (!GetClientRect(description.OutputWindow, &client)) return false;
        width = static_cast<UINT>(
            std::max<LONG>(0, client.right - client.left));
        height = static_cast<UINT>(
            std::max<LONG>(0, client.bottom - client.top));
    }
    return width == 2560 && height == 1440;
}

bool ReadCpuPerformanceTime(LARGE_INTEGER started, LARGE_INTEGER& ended,
    LARGE_INTEGER& frequency) noexcept
{
    return QueryPerformanceCounter(&ended) && QueryPerformanceFrequency(&frequency) &&
        frequency.QuadPart > 0 && ended.QuadPart >= started.QuadPart;
}

void CompleteCpuPerformanceSample(LARGE_INTEGER started) noexcept
{
    LARGE_INTEGER ended{};
    LARGE_INTEGER frequency{};
    if (!ReadCpuPerformanceTime(started, ended, frequency))
        return;
    const auto milliseconds = static_cast<double>(
        ended.QuadPart - started.QuadPart) * 1000.0 /
        static_cast<double>(frequency.QuadPart);
    const auto result = g_performance.cpu.AddSample(milliseconds);
    PublishPerformanceResult(PerformanceSampleSource::cpu, result, g_performance.cpu);
}

void ClearGpuPerformanceQueries() noexcept
{
    for (auto& slot : g_performance.gpuQueries)
    {
        slot.disjoint.Reset();
        slot.start.Reset();
        slot.end.Reset();
        slot.pending = false;
    }
    g_performance.gpuQueryCursor = 0;
    g_performance.gpuQueriesAvailable = false;
}

bool CreateGpuQuerySlot(ID3D11Device* device, GpuPerformanceQuerySlot& slot) noexcept
{
    D3D11_QUERY_DESC disjointDescription{
        .Query = D3D11_QUERY_TIMESTAMP_DISJOINT,
        .MiscFlags = 0
    };
    D3D11_QUERY_DESC timestampDescription{
        .Query = D3D11_QUERY_TIMESTAMP,
        .MiscFlags = 0
    };
    return !(FAILED(device->CreateQuery(&disjointDescription,
                &slot.disjoint)) ||
            FAILED(device->CreateQuery(&timestampDescription,
                &slot.start)) ||
            FAILED(device->CreateQuery(&timestampDescription,
                &slot.end)));
}

[[nodiscard]] bool EnsureGpuPerformanceQueries(
    ID3D11Device* device, bool measurePerformance) noexcept
{
    if (!measurePerformance) return false;
    if (device == nullptr) return false;
    if (g_performance.gpu.Complete()) return false;
    if (g_performance.gpuQueriesAvailable) return true;

    for (auto& slot : g_performance.gpuQueries)
    {
        if (!CreateGpuQuerySlot(device, slot))
        {
            ClearGpuPerformanceQueries();
            return false;
        }
    }
    g_performance.gpuQueriesAvailable = true;
    return true;
}

struct GpuQueryObservation
{
    ID3D11DeviceContext* context;
    GpuPerformanceQuerySlot& slot;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 started{};
    UINT64 ended{};
    HRESULT startResult{};
    HRESULT endResult{};

    bool ReadDisjoint() noexcept
    {
        const auto result = context->GetData(slot.disjoint.Get(), &disjoint,
            sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) return false;
        if (FAILED(result))
        {
            slot.pending = false;
            return false;
        }
        return true;
    }

    bool ReadTimestamps() noexcept
    {
        startResult = context->GetData(slot.start.Get(), &started,
            sizeof(started), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        endResult = context->GetData(slot.end.Get(), &ended,
            sizeof(ended), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (startResult == S_FALSE || endResult == S_FALSE) return false;
        slot.pending = false;
        return true;
    }

    bool SampleValid() const noexcept
    {
        return SUCCEEDED(startResult) && SUCCEEDED(endResult) && !disjoint.Disjoint &&
            disjoint.Frequency != 0 && ended >= started;
    }

    void Publish() const noexcept
    {
        const auto milliseconds = static_cast<double>(ended - started) *
            1000.0 / static_cast<double>(disjoint.Frequency);
        const auto result = g_performance.gpu.AddSample(milliseconds);
        PublishPerformanceResult(PerformanceSampleSource::gpu, result, g_performance.gpu);
    }

    void Poll() noexcept
    {
        if (!ReadDisjoint()) return;
        if (!ReadTimestamps()) return;
        if (SampleValid()) Publish();
    }
};

bool GpuPerformanceContextReady(ID3D11DeviceContext* context) noexcept
{
    return context != nullptr && g_performance.gpuQueriesAvailable && !g_performance.gpu.Complete();
}

void PollGpuPerformanceQueries(ID3D11DeviceContext* context) noexcept
{
    if (!GpuPerformanceContextReady(context)) return;
    for (auto& slot : g_performance.gpuQueries)
    {
        if (!slot.pending) continue;
        GpuQueryObservation{context, slot}.Poll();
    }
}

ScopedGpuPerformanceSample::ScopedGpuPerformanceSample(
    ID3D11DeviceContext* context) noexcept : context_(context)
{
    if (!GpuPerformanceContextReady(context_))
        return;
    for (std::size_t offset = 0;
         offset < g_performance.gpuQueries.size(); ++offset)
    {
        const auto index = (g_performance.gpuQueryCursor + offset) %
            g_performance.gpuQueries.size();
        auto& candidate = g_performance.gpuQueries[index];
        if (candidate.pending) continue;
        slot_ = &candidate;
        g_performance.gpuQueryCursor =
            (index + 1) % g_performance.gpuQueries.size();
        break;
    }
    if (slot_ == nullptr) return;
    context_->Begin(slot_->disjoint.Get());
    context_->End(slot_->start.Get());
}

ScopedGpuPerformanceSample::~ScopedGpuPerformanceSample() noexcept
{
    if (context_ == nullptr || slot_ == nullptr) return;
    context_->End(slot_->end.Get());
    context_->End(slot_->disjoint.Get());
    slot_->pending = true;
}

bool CpuPerformanceComplete() noexcept
{
    return g_performance.cpu.Complete();
}

void ResetPerformanceMeasurement() noexcept
{
    g_performance.cpu.Reset();
    g_performance.gpu.Reset();
    ClearGpuPerformanceQueries();
}

using Microsoft::WRL::ComPtr;

struct PresentFrame
{
    IDXGISwapChain* swapChain;
    bool developerHdrDescriptor;
    LARGE_INTEGER cpuPerformanceStarted{};
    bool measureCpuPerformance{};
    bool frameWasRedirected{};
    LinearColorDomain frameProxyDomain{LinearColorDomain::unknown};
    bool sdrColorSpaceEstablished{};
    bool displayChanged{};
    bool presentationReady{};
    bool gameplayTransition{};
    bool videoReady{};
    bool sdrContentReady{};
    bool sceneDomainMismatch{};
    bool temporalOutputRejected{};
    PipelineHeartbeatAction heartbeatAction{};
    bool rendered{};
    bool renderAttempted{};
    bool outputBlocked{};
    bool outputRetry{};
    bool hdrFallbackRendered{};
    bool publicRendered{};
    std::optional<SharedUiPresentation> publicPresentation;
    bool startupRendered{};
    bool convertedSdrRendered{};
    bool sdrHandoffRendered{};
    bool sdrHandoffDomainInvalid{};
    HdrActivationDecision activation{};

    bool TransitionResourceCreationFailed()
    {
        return HdrPathRequested() && developerHdrDescriptor &&
            g_referenceHealthy && !EnsureWideGamutResources(swapChain,
                g_rendererDevice.activeDevice, g_rendererDevice.activeContext);
    }

    bool PipboyObservationRetired()
    {
        return HdrSettingsFocusLossPending() ||
            g_pipboyRetiredContext.load(std::memory_order_acquire) ==
                g_pipboyScreen.state.contextGeneration ||
            g_evidence.resourceGeneration.load(std::memory_order_acquire) !=
                g_pipboyScreen.state.generation;
    }

    bool HasActiveContext()
    {
        return g_rendererDevice.activeDevice != nullptr && g_rendererDevice.activeContext != nullptr;
    }

    bool SceneCompositionReady()
    {
        if (sdrContentReady) return true;
        if (!g_scene.currentScene) return false;
        if (ApplicationPresentationFacadeActive() && !SharedUiFrameReady()) return false;
        return !g_configuration.productionRequested || g_scene.productionOperatorApplied || videoReady;
    }

    bool CanRenderHdr()
    {
        return !displayChanged && HdrPathRequested() &&
            developerHdrDescriptor && g_referenceHealthy &&
            (sdrContentReady || (g_productionHealthy &&
                heartbeatAction == PipelineHeartbeatAction::renderHdr)) &&
            SceneCompositionReady();
    }

    void RecordSceneContractRejections()
    {
        if (temporalOutputRejected)
            g_evidence.temporalOutputRejectedCount.fetch_add(
                1, std::memory_order_relaxed);
        if (sceneDomainMismatch)
        {
            g_evidence.colorDomainMismatchCount.fetch_add(
                1, std::memory_order_relaxed);
            RecordFailure(GraphicsObservationFailure::colorDomainMismatch);
        }
    }

    void PublishHeartbeatEvidence(const PipelineHeartbeatEvaluation& evaluation)
    {
        g_evidence.heartbeatReady.store(
            (evaluation.ready || videoReady) &&
                g_scene.currentScene != nullptr,
            std::memory_order_release);
        if (heartbeatAction == PipelineHeartbeatAction::renderHdr &&
            g_scene.currentScene != nullptr)
        {
            g_evidence.heartbeatReadyFrameCount.fetch_add(
                1, std::memory_order_relaxed);
            g_evidence.qualifiedSceneFrameCount.fetch_add(
                1, std::memory_order_relaxed);
            if (g_scene.temporalSceneReady)
                g_evidence.temporalAaFrameCount.fetch_add(
                    1, std::memory_order_relaxed);
            else
                g_evidence.temporalAaBypassFrameCount.fetch_add(
                    1, std::memory_order_relaxed);
        }
        else if (heartbeatAction == PipelineHeartbeatAction::waitInSdr)
        {
            g_evidence.heartbeatIncompleteFrameCount.fetch_add(
                1, std::memory_order_relaxed);
        }
        else
        {
            g_evidence.heartbeatInvalidFrameCount.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    void PublishHdr()
    {
        if (g_lifecycle.Snapshot().state ==
            HdrRendererState::vanillaSdr)
        {
            (void)g_lifecycle.PublishHdrResources();
            (void)PublishLifecycleGeneration();
        }
        else
        {
            (void)g_lifecycle.ResumeHdrPresentation();
        }
        g_evidence.hdrFrameCount.fetch_add(1, std::memory_order_relaxed);
        g_evidence.productionActive.store(
            activation.production, std::memory_order_release);
        if (activation.production)
            g_evidence.productionFrameCount.fetch_add(
                1, std::memory_order_relaxed);
        g_evidence.hdrChainActive.store(
            g_lifecycle.Snapshot().hdrActive,
            std::memory_order_release);
        g_evidence.presentationPaused.store(
            false, std::memory_order_release);
    }

    struct SdrTransitionResult
    {
        bool hardDisable;
        bool sdrApplied;
    };

    void ResetFailedPresentation(const SdrTransitionResult& transition)
    {
        if (outputRetry) return;
        ResetResources(!transition.sdrApplied, ApplicationPresentationFacadeActive());
        DXGI_SWAP_CHAIN_DESC description{};
        if (SUCCEEDED(swapChain->GetDesc(&description)))
        {
            g_presentation.width = description.BufferDesc.Width;
            g_presentation.height = description.BufferDesc.Height;
        }
    }

    bool PresentationWorkFailed() const
    {
        return renderAttempted || sdrHandoffDomainInvalid;
    }

    void UpdateSdrLifecycle(const SdrTransitionResult& transition)
    {
        if (transition.hardDisable && g_lifecycle.Snapshot().state ==
                               HdrRendererState::hdr10Ready)
        {
            (void)g_lifecycle.DeactivateToSdr();
            ResetFailedPresentation(transition);
            (void)PublishLifecycleGeneration();
        }
        else if (transition.hardDisable &&
                 PresentationWorkFailed())
        {
            ResetFailedPresentation(transition);
        }
        else if (!transition.hardDisable && g_lifecycle.Snapshot().state ==
                                     HdrRendererState::hdr10Ready)
        {
            (void)g_lifecycle.PauseHdrPresentation();
        }
    }

    bool CanRetryOutput() const noexcept
    {
        return outputRetry && !g_outputRecovery.used && g_rendererDevice.activeDevice &&
            SUCCEEDED(g_rendererDevice.activeDevice->GetDeviceRemovedReason());
    }

    void RecordOutputFailure()
    {
        g_evidence.referenceRenderFailureCount.fetch_add(1, std::memory_order_relaxed);
        if (CanRetryOutput()) g_outputRecovery = {true, true};
        else g_referenceHealthy = false;
        RecordFailure(GraphicsObservationFailure::referenceRenderFailed);
    }

    void RecordPresentationFailure()
    {
        if (renderAttempted || !sdrHandoffRendered)
            RecordOutputFailure();
    }

    void RecordProductionFailure()
    {
        if (HdrPathRequested() && heartbeatAction == PipelineHeartbeatAction::disableGeneration)
        {
            RecordFailure(
                temporalOutputRejected ?
                    GraphicsObservationFailure::temporalOutputUnavailable :
                sceneDomainMismatch ?
                    GraphicsObservationFailure::colorDomainMismatch :
                    GraphicsObservationFailure::requiredHeartbeatMissing);
        }
    }

    void PublishSdr()
    {
        const bool sdrApplied = !g_presentation.swapChain3 || SetColorSpace(false);
        if (!sdrApplied)
        {
            (void)RenderBlackPresentationFrame(swapChain,
                g_rendererDevice.activeDevice, g_rendererDevice.activeContext);
            sdrHandoffRendered = false;
            startupRendered = false;
        }
        const SdrTransitionResult transition{
            .hardDisable =
                renderAttempted || sdrHandoffDomainInvalid ||
                !sdrHandoffRendered,
            .sdrApplied = sdrApplied
        };
        UpdateSdrLifecycle(transition);
        g_evidence.sdrFallbackFrameCount.fetch_add(
            1, std::memory_order_relaxed);
        g_evidence.hdrChainActive.store(false, std::memory_order_release);
        g_evidence.productionActive.store(
            false, std::memory_order_release);
        g_evidence.presentationPaused.store(
            !transition.hardDisable && g_lifecycle.Snapshot().resourcesPublished,
            std::memory_order_release);
        RecordPresentationFailure();
    }

    void ObserveFrameThreads()
    {
        if (g_armor.armorEncodedActive)
        {
            CloseArmorEncodedCompositionForLifecycle();
            if (!g_armor.armorEncodedSharedSrgb) g_armor.armorEncodedFailed = true;
            if (g_referenceHealthy)
                g_armor.armorEncodedStatus = "Rejected frame: armor scope crossed Present";
        }
        const auto thread = GetCurrentThreadId();
        g_observedPresentThread.store(thread, std::memory_order_release);
        if (g_pipboyScreen.state.requested && PipboyObservationRetired())
            StopPipboyScreenObservation("Stopped: focus loss or context/resource retirement");
        const auto drawThread =
            g_currentFrameQualifiedDrawThread.exchange(
                0, std::memory_order_acq_rel);
        const auto drawThreadConflict =
            g_currentFrameQualifiedDrawThreadConflict.exchange(
                false, std::memory_order_acq_rel);
        const auto liveSettingsCompatible =
            LiveSettingsFrameThreadsCompatible(
                drawThread, drawThreadConflict, thread);
        g_liveSettingsThreadCompatible.store(liveSettingsCompatible,
            std::memory_order_release);
    }

    void BeginFrame()
    {
        measureCpuPerformance =
            !CpuPerformanceComplete() &&
            PerformanceResolutionMatches(swapChain, g_configuration.measurePerformance) &&
            QueryPerformanceCounter(&cpuPerformanceStarted);
        g_evidence.presentCount.fetch_add(1, std::memory_order_relaxed);
        // Snapshot the already-drawn frame before any display/game lifecycle
        // transition may retire its proxy and semantic state. Present still
        // owes that redirected frame a current backbuffer result even when the
        // new generation cannot render HDR.
        frameWasRedirected =
            g_presentation.presentationRedirectedThisFrame || ApplicationPresentationFacadeActive();
        frameProxyDomain =
            g_presentation.presentationProxyDomain;
    }

    void ApplyLifecycle()
    {
        displayChanged = DisplayGenerationChanged(
            swapChain, sdrColorSpaceEstablished);
        if (displayChanged) g_outputRecovery = {};
        const auto gameLifecycle = ReadGraphicsLifecycle();
        presentationReady = gameLifecycle.gameplayReady &&
            !gameLifecycle.presentationBlocked;
        gameplayTransition = ApplyGraphicsLifecycle(gameLifecycle);
        const bool resourceCheckFailed = g_gameplayResourceCheckPending && presentationReady &&
            TransitionResourceCreationFailed();
        if (resourceCheckFailed)
            RecordProductionResourceFailure(swapChain);
        g_gameplayResourceCheckPending = false;
        RecoverOutput();
    }

    void RecoverOutput()
    {
        if (!g_outputRecovery.pending) return;
        g_outputRecovery.pending = false;
        if (!RecreatePresentationConstants(g_rendererDevice.activeDevice))
            g_referenceHealthy = false;
    }

    bool GameplayFrameReady() const
    {
        return presentationReady && !gameplayTransition;
    }

    bool IsVideoFrameReady()
    {
        return GameplayFrameReady() &&
            g_scene.videoSceneReady &&
            g_scene.currentScene != nullptr;
    }

    PipelineHeartbeatAction BaseHeartbeatAction(const PipelineHeartbeatEvaluation& evaluation)
    {
        return videoReady ?
            PipelineHeartbeatAction::renderHdr :
            (GameplayFrameReady() ?
                DecidePipelineHeartbeatAction(evaluation) :
                PipelineHeartbeatAction::waitInSdr);
    }

    bool HasSceneDomainMismatch()
    {
        if (ApplicationPresentationFacadeActive() && SharedUiFrameEncoded()) return false;
        const auto requiredSceneDomain =
            (g_configuration.productionRequested || videoReady) ?
                LinearColorDomain::linearBt2020UiRelative :
                LinearColorDomain::linearBt709Relative;
        return
            g_scene.currentScene != nullptr &&
            g_scene.currentSceneDomain != requiredSceneDomain;
    }

    void EvaluateHeartbeat()
    {
        const auto generation =
            g_evidence.resourceGeneration.load(std::memory_order_acquire);
        const auto evaluation = g_heartbeat.Evaluate(generation, g_frame);
        videoReady = IsVideoFrameReady();
        const auto baseHeartbeatAction = BaseHeartbeatAction(evaluation);
        sceneDomainMismatch = HasSceneDomainMismatch();
        temporalOutputRejected =
            !videoReady && g_scene.temporalPassObserved &&
            !g_scene.temporalSceneReady;
        heartbeatAction =
            (temporalOutputRejected || sceneDomainMismatch) ?
            PipelineHeartbeatAction::disableGeneration :
            baseHeartbeatAction;
        RecordProductionFailure();
        RecordSceneContractRejections();
        PublishHeartbeatEvidence(evaluation);
    }

    void RenderHdr()
    {
        sdrContentReady = !gameplayTransition && SharedSdrFrameReady();
        if (CanRenderHdr())
        {
            const auto result = RenderReferenceFrame(swapChain,
                g_rendererDevice.activeDevice, g_rendererDevice.activeContext);
            RecordOutputResult(result);
            rendered = result == PresentationResult::rendered;
        }
    }

    void RecordOutputResult(PresentationResult result) noexcept
    {
        outputBlocked = result == PresentationResult::blocked;
        outputRetry = result == PresentationResult::retry;
        renderAttempted = result == PresentationResult::rendered ||
            result == PresentationResult::failed || outputRetry;
    }

    void RenderBlackHandoff()
    {
        // A redirected HDR-domain proxy is never relabeled as SDR.
        // A bounded black current frame avoids stale PQ content while
        // an ordinary transition waits for a new qualified scene.
        if (sdrHandoffDomainInvalid)
        {
            g_evidence.colorDomainMismatchCount.fetch_add(
                1, std::memory_order_relaxed);
            g_referenceHealthy = false;
            RecordFailure(
                GraphicsObservationFailure::colorDomainMismatch);
        }
        sdrHandoffRendered = RenderBlackPresentationFrame(swapChain,
            g_rendererDevice.activeDevice, g_rendererDevice.activeContext) &&
            (sdrColorSpaceEstablished || SetColorSpace(false));
    }

    bool CanRenderHdrFallback()
    {
        const auto state = g_lifecycle.Snapshot().state;
        const std::array ready{ApplicationPresentationFacadeActive() || frameWasRedirected, !displayChanged,
            HdrPathRequested(), developerHdrDescriptor, g_referenceHealthy.load(),
            !renderAttempted,
            HasActiveContext(), g_presentation.presentationProxy != nullptr,
            g_presentation.presentationProxySrv != nullptr,
            g_presentation.swapChain3 != nullptr, g_presentation.colorSpaceSupported,
            g_presentation.r10Presentation, g_presentation.flipModel,
            g_presentation.bufferCount >= 2,
            state == HdrRendererState::vanillaSdr || state == HdrRendererState::hdr10Ready};
        return std::ranges::all_of(ready, [](bool value) { return value; });
    }

    bool RenderReadableHdrFallback()
    {
        if (outputBlocked) return false;
        if (RenderPublicFrame()) return true;
        if (outputBlocked) return false;
        if (RenderStartupFrame()) return true;
        if (outputBlocked) return false;
        return RenderEmergencyFrame();
    }

    void RenderHdrFallback()
    {
        if (rendered || !CanRenderHdrFallback()) return;
        if (RenderReadableHdrFallback()) return;
        hdrFallbackRendered = RenderBlackPresentationFrame(swapChain,
            g_rendererDevice.activeDevice, g_rendererDevice.activeContext) && SetColorSpace(true);
        renderAttempted = !hdrFallbackRendered;
    }

    bool RenderStartupFrame()
    {
        const auto startup = SharedUiStartupPresentation();
        if (!startup) return false;
        const auto result = RenderSdrPresentationProxy(swapChain,
            g_rendererDevice.activeDevice, g_rendererDevice.activeContext, &*startup);
        RecordOutputResult(result);
        hdrFallbackRendered = result == PresentationResult::rendered;
        startupRendered = hdrFallbackRendered;
        return renderAttempted;
    }

    bool RenderPublicFrame()
    {
        const auto presentation = SharedUiPresentationForOutput();
        if (!presentation) return false;
        return RenderPublicPresentation(*presentation);
    }

    bool RenderEmergencyFrame()
    {
        const auto presentation = SharedUiEmergencyPresentation();
        if (!presentation) return false;
        return RenderPublicPresentation(*presentation);
    }

    bool RenderPublicPresentation(const SharedUiPresentation& presentation)
    {
        const auto result = RenderPublicSharedUiFrame(swapChain,
            g_rendererDevice.activeDevice, g_rendererDevice.activeContext, presentation);
        RecordOutputResult(result);
        publicRendered = result == PresentationResult::rendered;
        hdrFallbackRendered = publicRendered;
        if (publicRendered) publicPresentation = presentation;
        return renderAttempted;
    }

    bool ConvertPublicImageToSdr()
    {
        const auto presentation = SharedUiEmergencyPresentation();
        if (!presentation) return false;
        const auto result = RenderPublicSharedUiSdrFrame(swapChain,
            g_rendererDevice.activeDevice, g_rendererDevice.activeContext, *presentation);
        RecordOutputResult(result);
        convertedSdrRendered = result == PresentationResult::rendered;
        if (convertedSdrRendered)
        {
            publicPresentation = presentation;
            renderAttempted = false;
        }
        return convertedSdrRendered;
    }

    bool CopyVanillaSdr()
    {
        const auto result = RenderSdrPresentationProxy(swapChain,
            g_rendererDevice.activeDevice, g_rendererDevice.activeContext);
        RecordOutputResult(result);
        startupRendered = result == PresentationResult::rendered;
        if (startupRendered) renderAttempted = false;
        return startupRendered;
    }

    void RenderSdrHandoff()
    {
        const auto sdrHandoffAction = HandoffAction();
        sdrHandoffRendered = sdrHandoffAction == SdrHandoffAction::none;
        sdrHandoffDomainInvalid =
            sdrHandoffAction == SdrHandoffAction::clearBlackAndDisable;
        if (sdrHandoffRendered || !HasActiveContext()) return;
        if (sdrHandoffAction == SdrHandoffAction::convertPublicImage)
            sdrHandoffRendered = ConvertPublicImageToSdr();
        else if (sdrHandoffAction == SdrHandoffAction::copyVanillaSdr)
            sdrHandoffRendered = CopyVanillaSdr();
        if (!sdrHandoffRendered) RenderBlackHandoff();
    }

    SdrHandoffAction LegacyHandoffAction() const noexcept
    {
        if (frameWasRedirected && SharedUiEmergencyPresentation())
            return SdrHandoffAction::convertPublicImage;
        return DecideSdrHandoff(rendered, frameWasRedirected, frameProxyDomain,
            g_presentation.presentationProxySrv != nullptr);
    }

    SdrHandoffAction HandoffAction() const noexcept
    {
        if (rendered || hdrFallbackRendered) return SdrHandoffAction::none;
        if (outputBlocked) return SdrHandoffAction::clearBlack;
        if (renderAttempted) return SdrHandoffAction::clearBlack;
        if (!ApplicationPresentationFacadeActive())
            return LegacyHandoffAction();
        if (SharedUiStartupSdr()) return SdrHandoffAction::copyVanillaSdr;
        if (SharedUiEmergencyPresentation()) return SdrHandoffAction::convertPublicImage;
        return SdrHandoffAction::clearBlack;
    }

    bool ProductionColorReady() const noexcept
    {
        if (sdrContentReady) return g_configuration.productionRequested;
        const bool contentReady = g_scene.productionOperatorApplied || videoReady;
        return g_configuration.productionRequested && contentReady &&
            g_replacements.operatorConstants.Get() != nullptr;
    }

    bool ActivationQualityRejected() const noexcept
    {
        return activation.reason == HdrActivationReason::productionToneMapperUnavailable ||
            activation.reason == HdrActivationReason::signalContractNotReady;
    }

    void ValidateActivation()
    {
        activation = DecideHdrActivation(
            {
                .enabled = rendered,
                .developerMode = g_configuration.developerMode,
                .developerReferenceRequested =
                    g_configuration.developerHdr10Reference,
                .productionRequested =
                    g_configuration.productionRequested
            },
            {
                .runtimeAuthorized = true,
                .signalContractReady =
                    sdrContentReady || heartbeatAction == PipelineHeartbeatAction::renderHdr,
                .fp16WorkingImage = sdrContentReady || g_scene.currentScene != nullptr,
                .r10g10b10a2Presentation =
                    g_presentation.r10Presentation &&
                    g_presentation.presentationBackBufferRtv.Get() != nullptr,
                .flipModel = g_presentation.flipModel,
                .bufferCount = g_presentation.bufferCount,
                .pqBt2020PresentSupported =
                    g_presentation.colorSpaceSupported,
                .pqBt2020Applied =
                    g_evidence.hdrColorSpaceActive.load(),
                .resourcesReady = rendered,
                .productionToneMapperReady = ProductionColorReady()
            });
        rendered = rendered && activation.hdrActive &&
            (activation.production == g_configuration.productionRequested);
        if (!rendered && ActivationQualityRejected())
            renderAttempted = false;
    }

    void PublishHdrFallback()
    {
        // Valid HDR10 transport does not establish a qualified scene or production frame.
        (void)g_lifecycle.PauseHdrPresentation();
        g_evidence.hdrFrameCount.fetch_add(1, std::memory_order_relaxed);
        g_evidence.hdrChainActive.store(false, std::memory_order_release);
        g_evidence.productionActive.store(false, std::memory_order_release);
        g_evidence.presentationPaused.store(
            g_lifecycle.Snapshot().resourcesPublished, std::memory_order_release);
    }

    bool ReadableOutputRendered() const noexcept
    {
        return rendered || publicRendered || startupRendered || convertedSdrRendered;
    }

    bool RecoveredOutputFailure() const noexcept
    {
        return g_outputRecovery.used && g_evidence.failure.load() ==
            static_cast<std::uint32_t>(GraphicsObservationFailure::referenceRenderFailed);
    }

    void CompleteOutputRecovery()
    {
        if (!ReadableOutputRendered()) return;
        if (RecoveredOutputFailure())
            RecordFailure(GraphicsObservationFailure::none);
        g_outputRecovery = {};
    }

    void PublishResult()
    {
        CompleteOutputRecovery();
        if (rendered)
        {
            PublishHdr();
        }
        else if (hdrFallbackRendered)
        {
            PublishHdrFallback();
        }
        else
        {
            PublishSdr();
        }
    }

    bool PerformanceSampleEligible() const
    {
        return !g_evidence.performanceActivePath.load(
            std::memory_order_acquire) || rendered;
    }

    void CompleteFrameMeasurements()
    {
        if (measureCpuPerformance &&
            PerformanceSampleEligible())
            CompleteCpuPerformanceSample(cpuPerformanceStarted);
    }

    void AdvanceFrame()
    {
        ResetSceneTargetInitialization();
        g_scene.currentScene.Reset();
        g_scene.currentSceneDomain = LinearColorDomain::unknown;
        g_presentation.presentationProxyDomain = LinearColorDomain::unknown;
        g_evidence.colorDomain.store(
            static_cast<std::uint32_t>(LinearColorDomain::unknown),
            std::memory_order_release);
        g_scene.temporalPassObserved = false;
        g_scene.temporalSceneReady = false;
        g_scene.videoSceneReady = false;
        g_scene.productionOperatorApplied = false;
        g_presentation.presentationRedirectedThisFrame = false;
        ++g_frame;
        g_lutInspectorFrame = 0;
        g_lutInspectorValidatedSlots = 0;
        g_heartbeat.BeginFrame(
            g_evidence.resourceGeneration.load(std::memory_order_acquire),
            g_frame);
    }

    bool Prepare()
    {
        ObserveFrameThreads();
        BeginFrame();
        ApplyLifecycle();
        EvaluateHeartbeat();
        RenderHdr();
        ValidateActivation();
        RenderHdrFallback();
        RenderSdrHandoff();
        PublishResult();
        if (rendered) PublishSharedUiPresentation();
        TraceSharedUiOutput({rendered, rendered || hdrFallbackRendered || sdrHandoffRendered,
            startupRendered, publicRendered}, publicPresentation ? &*publicPresentation : nullptr);
        CompleteFrameMeasurements();
        return rendered;
    }

    void Complete(bool commitSettings)
    {
        CompleteSharedUiFrame(rendered);
        if (commitSettings)
        {
            CommitLiveSettingsAtEndPresent(
                g_rendererDevice.activeDevice,
                g_rendererDevice.activeContext);
            CommitDeveloperExposureAtEndPresent(g_rendererDevice.activeDevice,
                g_rendererDevice.activeContext, rendered);
        }
        AdvanceFrame();
    }
};

[[nodiscard]] bool PreparePresentFrame(IDXGISwapChain* swapChain, bool developerHdrDescriptor)
{
    return PresentFrame{swapChain, developerHdrDescriptor}.Prepare();
}

void CompletePresentFrame(bool rendered, bool commitSettings)
{
    PresentFrame frame{};
    frame.rendered = rendered;
    frame.Complete(commitSettings);
}

} // namespace fo4hdr
