// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "GraphicsDiagnostics.hpp"
#include "FrameTrace.hpp"
#include "ShaderRegistry.hpp"
#include "hooks/DeviceHooks.hpp"
#include <algorithm>
#include <utility>
#include "RendererLifetime.hpp"
#include "LiveSettings.hpp"
#include <FO4Isotope/ui/HdrSettingsInput.hpp>
#include <format>
#include <string>

namespace fo4hdr {
AtomicEvidence g_evidence;
void RecordFailure(GraphicsObservationFailure value) noexcept
{
    g_evidence.failure.store(
        static_cast<std::uint32_t>(value), std::memory_order_release);
}

void RecordAbiException() noexcept
{
    g_evidence.abiExceptionCount.fetch_add(1, std::memory_order_relaxed);
    RecordFailure(GraphicsObservationFailure::abiException);
}


namespace
{
void ReadActivationSnapshot(GraphicsObservationSnapshot& snapshot) noexcept
{
    snapshot.compiledIn = true;
    snapshot.requested = g_evidence.requested.load();
    snapshot.initialized = g_evidence.initialized.load();
    snapshot.developerReferenceRequested = g_evidence.referenceRequested.load();
    snapshot.productionRequested = g_evidence.productionRequested.load();
    snapshot.productionActive = g_evidence.productionActive.load();
    snapshot.hdrChainActive = g_evidence.hdrChainActive.load();
    snapshot.hdrColorSpaceActive = g_evidence.hdrColorSpaceActive.load();
    snapshot.heartbeatReady = g_evidence.heartbeatReady.load();
    snapshot.gameplayReady = g_evidence.gameplayReady.load();
    snapshot.gameResetEpoch = g_evidence.gameResetEpoch.load();
    snapshot.presentationPaused = g_evidence.presentationPaused.load();
    snapshot.performanceRequested = g_evidence.performanceRequested.load();
    snapshot.performanceActivePath = g_evidence.performanceActivePath.load();
    snapshot.performanceCpuComplete = g_evidence.performanceCpuComplete.load();
    snapshot.performanceGpuComplete = g_evidence.performanceGpuComplete.load();
    snapshot.performanceCpuBudgetPassed = g_evidence.performanceCpuBudgetPassed.load();
    snapshot.performanceGpuBudgetPassed = g_evidence.performanceGpuBudgetPassed.load();
}

void ReadOwnershipSnapshot(GraphicsObservationSnapshot& snapshot, HookOwnershipState health) noexcept
{
    snapshot.d3dIatOwned = health == HookOwnershipState::owned;
    snapshot.d3dIatDisplaced = health == HookOwnershipState::ownershipLost;
    snapshot.liveSettingsThreadCompatible = g_liveSettingsThreadCompatible.load();
    snapshot.qualifiedDrawThreadId = g_observedQualifiedDrawThread.load();
    snapshot.presentThreadId = g_observedPresentThread.load();
    snapshot.failure = static_cast<GraphicsObservationFailure>( g_evidence.failure.load());
}

void ReadDeviceCountersSnapshot(GraphicsObservationSnapshot& snapshot) noexcept
{
    snapshot.deviceCreateCount = g_evidence.deviceCreateCount.load();
    snapshot.deviceNextCallCount = g_evidence.deviceNextCallCount.load();
    snapshot.successfulDeviceCreateCount = g_evidence.successfulDeviceCreateCount.load();
    snapshot.deviceShadowCount = g_evidence.deviceShadowCount.load();
    snapshot.deviceShadowRetireCount = g_evidence.deviceShadowRetireCount.load();
    snapshot.deviceShadowInstallFailureCount = g_evidence.deviceShadowInstallFailureCount.load();
    snapshot.pixelShaderCreateCount = g_evidence.pixelShaderCreateCount.load();
    snapshot.knownPixelShaderCreateCount = g_evidence.knownPixelShaderCreateCount.load();
    snapshot.unknownPixelShaderCreateCount = g_evidence.unknownPixelShaderCreateCount.load();
    snapshot.protectionRestoreFailureCount = g_evidence.protectionRestoreFailureCount.load();
    snapshot.abiExceptionCount = g_evidence.abiExceptionCount.load();
    snapshot.hdrCreationAttemptCount = g_evidence.hdrCreationAttemptCount.load();
    snapshot.hdrCreationFallbackCount = g_evidence.hdrCreationFallbackCount.load();
    snapshot.hdrCreationFallbackSuccessCount = g_evidence.hdrCreationFallbackSuccessCount.load();
}

void ReadPresentationCountersSnapshot(GraphicsObservationSnapshot& snapshot) noexcept
{
    snapshot.promotedResourceCount = g_evidence.promotedResourceCount.load();
    snapshot.presentationProxyCreateCount = g_evidence.presentationProxyCreateCount.load();
    snapshot.presentationRedirectCount = g_evidence.presentationRedirectCount.load();
    snapshot.presentCount = g_evidence.presentCount.load();
    snapshot.resizeCount = g_evidence.resizeCount.load();
    snapshot.hdrFrameCount = g_evidence.hdrFrameCount.load();
    snapshot.sdrFallbackFrameCount = g_evidence.sdrFallbackFrameCount.load();
    snapshot.heartbeatReadyFrameCount = g_evidence.heartbeatReadyFrameCount.load();
    snapshot.heartbeatIncompleteFrameCount = g_evidence.heartbeatIncompleteFrameCount.load();
    snapshot.heartbeatInvalidFrameCount = g_evidence.heartbeatInvalidFrameCount.load();
    snapshot.qualifiedSceneFrameCount = g_evidence.qualifiedSceneFrameCount.load();
}

void ReadSceneCountersSnapshot(GraphicsObservationSnapshot& snapshot) noexcept
{
    snapshot.temporalAaDrawCount = g_evidence.temporalAaDrawCount.load();
    snapshot.temporalUnqualifiedDrawCount = g_evidence.temporalUnqualifiedDrawCount.load();
    snapshot.temporalProxyDrawCount = g_evidence.temporalProxyDrawCount.load();
    snapshot.temporalMissingProxyDrawCount = g_evidence.temporalMissingProxyDrawCount.load();
    snapshot.temporalAaFrameCount = g_evidence.temporalAaFrameCount.load();
    snapshot.temporalAaBypassFrameCount = g_evidence.temporalAaBypassFrameCount.load();
    snapshot.temporalOutputRejectedCount = g_evidence.temporalOutputRejectedCount.load();
    snapshot.referenceRenderFailureCount = g_evidence.referenceRenderFailureCount.load();
    snapshot.productionOperatorDrawCount = g_evidence.productionOperatorDrawCount.load();
    snapshot.productionFrameCount = g_evidence.productionFrameCount.load();
    snapshot.nativeUiBackdropCaptureDrawCount = g_evidence.nativeUiBackdropCaptureDrawCount.load();
    snapshot.nativeUiBackdropCaptureFrameCount = g_evidence.nativeUiBackdropCaptureFrameCount.load();
    snapshot.nativeUiCaptureDrawCount = g_evidence.nativeUiCaptureDrawCount.load();
    snapshot.nativeUiCaptureFrameCount = g_evidence.nativeUiCaptureFrameCount.load();
}

void ReadPerformanceCountersSnapshot(GraphicsObservationSnapshot& snapshot) noexcept
{
    snapshot.performanceCpuMeasuredSamples = g_evidence.performanceCpuMeasuredSamples.load();
    snapshot.performanceGpuMeasuredSamples = g_evidence.performanceGpuMeasuredSamples.load();
    snapshot.performanceCpuP95Nanoseconds = g_evidence.performanceCpuP95Nanoseconds.load();
    snapshot.performanceCpuP99Nanoseconds = g_evidence.performanceCpuP99Nanoseconds.load();
    snapshot.performanceGpuP95Nanoseconds = g_evidence.performanceGpuP95Nanoseconds.load();
    snapshot.performanceGpuP99Nanoseconds = g_evidence.performanceGpuP99Nanoseconds.load();
}

void ReadResourceStateSnapshot(GraphicsObservationSnapshot& snapshot) noexcept
{
    snapshot.resourceGeneration = g_evidence.resourceGeneration.load();
    snapshot.colorDomainGeneration = g_evidence.colorDomainGeneration.load();
    snapshot.colorDomainMismatchCount = g_evidence.colorDomainMismatchCount.load();
    snapshot.colorDomain = static_cast<LinearColorDomain>( g_evidence.colorDomain.load());
    snapshot.knownShaderMask = g_evidence.knownShaderMask.load();
    snapshot.lastKnownShaderCrc32 = g_evidence.lastKnownShaderCrc32.load();
    snapshot.lastDeviceResult = g_evidence.lastDeviceResult.load();
    snapshot.lastPixelShaderResult = g_evidence.lastPixelShaderResult.load();
}

}  // namespace

GraphicsObservationSnapshot ReadGraphicsObservation() noexcept
{
    const auto health = GraphicsImportHealth();
    GraphicsObservationSnapshot snapshot{};
    ReadActivationSnapshot(snapshot);
    ReadOwnershipSnapshot(snapshot, health);
    ReadDeviceCountersSnapshot(snapshot);
    ReadPresentationCountersSnapshot(snapshot);
    ReadSceneCountersSnapshot(snapshot);
    ReadPerformanceCountersSnapshot(snapshot);
    ReadResourceStateSnapshot(snapshot);
    return snapshot;
}

const char* GraphicsObservationFailureName(GraphicsObservationFailure value) noexcept
{
    constexpr std::array names{
        std::pair{GraphicsObservationFailure::none, "none"},
        std::pair{GraphicsObservationFailure::imageUnavailable, "image-unavailable"},
        std::pair{GraphicsObservationFailure::importUnavailable, "import-unavailable"},
        std::pair{GraphicsObservationFailure::uncallableTarget, "uncallable-target"},
        std::pair{GraphicsObservationFailure::iatInstallFailed, "iat-install-failed"},
        std::pair{GraphicsObservationFailure::selfReferentialNext, "self-referential-next"},
        std::pair{GraphicsObservationFailure::registryBusy, "registry-busy"},
        std::pair{GraphicsObservationFailure::registryExhausted, "registry-exhausted"},
        std::pair{GraphicsObservationFailure::protectionRestoreFailed, "protection-restore-failed"},
        std::pair{GraphicsObservationFailure::deviceShadowFailed, "device-shadow-failed"},
        std::pair{GraphicsObservationFailure::distinctInterfaceIdentity, "distinct-interface-identity"},
        std::pair{GraphicsObservationFailure::abiException, "abi-exception"},
        std::pair{GraphicsObservationFailure::descriptorUnavailable, "descriptor-unavailable"},
        std::pair{GraphicsObservationFailure::hdrAttemptFailed, "hdr-attempt-failed"},
        std::pair{GraphicsObservationFailure::swapChainShadowFailed, "swap-chain-shadow-failed"},
        std::pair{GraphicsObservationFailure::contextShadowFailed, "context-shadow-failed"},
        std::pair{GraphicsObservationFailure::shaderCreationFailed, "shader-creation-failed"},
        std::pair{GraphicsObservationFailure::resourceCreationFailed, "resource-creation-failed"},
        std::pair{GraphicsObservationFailure::colorSpaceUnsupported, "color-space-unsupported"},
        std::pair{GraphicsObservationFailure::colorSpaceRejected, "color-space-rejected"},
        std::pair{GraphicsObservationFailure::requiredHeartbeatMissing, "required-heartbeat-missing"},
        std::pair{GraphicsObservationFailure::temporalOutputUnavailable, "temporal-output-unavailable"},
        std::pair{GraphicsObservationFailure::referenceRenderFailed, "reference-render-failed"},
        std::pair{GraphicsObservationFailure::colorDomainMismatch, "color-domain-mismatch"},
        std::pair{GraphicsObservationFailure::deviceLost, "device-lost"}
    };
    const auto found = std::ranges::find(names, value, &decltype(names)::value_type::first);
    return found == names.end() ? "unknown" : found->second;
}

using Microsoft::WRL::ComPtr;

PipboyScreenResources g_pipboyScreen;
std::atomic<std::uint64_t> g_pipboyRetiredContext{};
std::atomic<std::uint64_t> g_pipboyWatchedContext{};
void StopPipboyScreenObservation(const char* reason) noexcept
{
    g_pipboyWatchedContext.store(0, std::memory_order_release);
    g_pipboyScreen.state.Stop();
    g_pipboyScreen.producerIdentity.Reset();
    g_pipboyScreen.status = reason;
}

bool PipboyTextureMatches(const D3D11_TEXTURE2D_DESC& d) noexcept
{
    const std::array layout{d.Width, d.Height, d.MipLevels, d.ArraySize,
        d.SampleDesc.Count, d.SampleDesc.Quality};
    constexpr std::array expectedLayout{876U, 700U, 1U, 1U, 1U, 0U};
    return layout == expectedLayout && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool ReadPipboyScreenResource(ID3D11Resource* resource,
    PipboyScreenViewObservation& observed, ComPtr<IUnknown>& identity) noexcept
{
    ComPtr<ID3D11Texture2D> texture;
    if (!resource) return false;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) ||
        FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
    texture->GetDesc(&observed.texture);
    return PipboyTextureMatches(observed.texture);
}

struct PipboyDrawObservation
{
    ID3D11DeviceContext* context;
    PipboyScreenResources& p;
    bool producer;
    PipboyScreenViewObservation& input;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Resource> resource;
    ComPtr<IUnknown> inputIdentity;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IUnknown> outputIdentity;
    ComPtr<ID3D11Predicate> predicate;

    void Reject(std::size_t index, const char* reason) noexcept
    {
        ++p.state.rejectedDraws;
        ++p.rejections[index];
        p.status = reason;
        if (producer) p.producerIdentity.Reset();
    }

    bool InputDescriptorMatches() const noexcept
    {
        return input.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            input.dimension == D3D11_SRV_DIMENSION_TEXTURE2D && input.mip == 0 &&
            (input.mipCount == 1 || input.mipCount == UINT(-1));
    }

    bool OutputDescriptorMatches() const noexcept
    {
        const auto& output = p.producerOutput;
        return output.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            output.dimension == D3D11_RTV_DIMENSION_TEXTURE2D && output.mip == 0;
    }

    void ReadInputDescriptor() noexcept
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        srv->GetDesc(&sd);
        input.format = sd.Format;
        input.dimension = sd.ViewDimension;
        if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
        {
            input.mip = sd.Texture2D.MostDetailedMip;
            input.mipCount = sd.Texture2D.MipLevels;
        }
    }

    void ReadOutputDescriptor() noexcept
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rtv->GetDesc(&rd);
        auto& output = p.producerOutput;
        output.frame = g_frame;
        output.format = rd.Format;
        output.dimension = rd.ViewDimension;
        if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) output.mip = rd.Texture2D.MipSlice;
    }

    bool ReadInput() noexcept
    {

        context->PSGetShaderResources(producer ? 0 : 4, 1, &srv);
        if (!srv) { Reject(0, "Rejected: missing SRV"); return false; }
        ReadInputDescriptor();
        srv->GetResource(&resource);
        const bool inputResource = ReadPipboyScreenResource(resource.Get(), input, inputIdentity);
        if (!inputResource || !InputDescriptorMatches())
        { Reject(1, "Rejected: SRV descriptor/resource"); return false; }
        return true;
    }

    void ObserveConsumer() noexcept
    {
        const bool matched = p.producerIdentity && p.producerIdentity.Get() == inputIdentity.Get();
        p.state.Consumer(g_frame, matched, p.producerIdentity != nullptr);
        p.status = matched ? "Observed: consumer uses latest producer resource" :
            (p.producerIdentity ? "Observed: consumer resource differs from latest producer" :
                "Pending: consumer precedes an observed producer");
    }

    bool ReadOutput() noexcept
    {

        context->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) { Reject(2, "Rejected: missing RTV0"); return false; }
        ReadOutputDescriptor();
        auto& output = p.producerOutput;
        resource.Reset();

        rtv->GetResource(&resource);
        const bool outputResource = ReadPipboyScreenResource(resource.Get(), output, outputIdentity);
        if (!outputResource || !OutputDescriptorMatches())
        { Reject(3, "Rejected: RTV descriptor/resource"); return false; }
        if (outputIdentity.Get() == inputIdentity.Get())
        { Reject(4, "Rejected: producer input aliases output"); return false; }
        return true;
    }

    void CompleteProducer() noexcept
    {

        BOOL predicateValue{};
        context->GetPredication(&predicate, &predicateValue);
        if (predicate) { Reject(5, "Rejected: predicated producer"); return; }
        PublishProducer();
    }

    void PublishProducer() noexcept
    {
        // Native Draw calls leave bindings unchanged. Observe after forwarding so
        // a consumer earlier next frame can refer to the completed producer draw.
        // This retains object lineage only: other writes and complete draw coverage
        // are not tracked, so the match never authorizes a color transformation.
        if (p.producerIdentity && p.producerIdentity.Get() != outputIdentity.Get())
            ++p.producerResourceChanges;
        p.producerIdentity = std::move(outputIdentity);
        p.state.Producer(g_frame);
        p.status = "Observed: completed producer submission";
    }

    void Observe() noexcept
    {
        input = {};
        input.frame = g_frame;
        if (producer) p.producerOutput = {};
        if (!ReadInput()) return;
        if (!producer)
        {
            ObserveConsumer();
            return;
        }
        if (!ReadOutput()) return;
        CompleteProducer();
    }
};
bool PipboyRendererReady() noexcept
{
    return g_configuration.developerMode && g_referenceHealthy &&
        g_gameplayReadyApplied.load(std::memory_order_acquire) &&
        g_evidence.hdrColorSpaceActive.load(std::memory_order_acquire);
}

bool PipboySessionRetired(const DrawObservation& record, const PipboyScreenResources& p) noexcept
{
    return !PipboyRendererReady() ||
        g_pipboyRetiredContext.load(std::memory_order_acquire) == p.state.contextGeneration ||
        !p.state.Matches(g_evidence.resourceGeneration.load(std::memory_order_acquire),
            record.generation);
}

bool ReadPipboyShaderRole(ID3D11PixelShader* shader, PipboyScreenShader& role) noexcept
{
    UINT size = sizeof(role);
    if (!shader) return false;
    if (FAILED(shader->GetPrivateData(kPipboyScreenShaderRole, &size, &role))) return false;
    return size == sizeof(role) &&
        (role == PipboyScreenShader::producer || role == PipboyScreenShader::consumer);
}
bool ValidatePipboySession(ID3D11DeviceContext* context, const DrawObservation& record) noexcept
{
    auto& p = g_pipboyScreen;
    if (!p.state.requested) return false;
    if (HdrSettingsFocusLossPending())
    {
        StopPipboyScreenObservation("Stopped: focus loss");
        return false;
    }
    if (context != g_rendererDevice.activeContext || !record.immediate) return false;
    if (PipboySessionRetired(record, p))
    {
        StopPipboyScreenObservation("Stopped: lifecycle or generation changed");
        return false;
    }
    return true;
}

void ObservePipboyShaderRole(ID3D11DeviceContext* context, PipboyScreenResources& p,
    PipboyScreenShader role) noexcept
{
    const bool producer = role == PipboyScreenShader::producer;
    auto& input = producer ? p.producerInput : p.consumerInput;
    PipboyDrawObservation observation{context, p, producer, input};
    observation.Observe();
}

void ObservePipboyScreenDraw(ID3D11DeviceContext* context,
    const DrawObservation& record) noexcept
{
    if (g_pipboyWatchedContext.load(std::memory_order_acquire) == 0) return;
    // The UI and retained identity belong to the presenting thread. Foreign
    // callbacks must not even read their non-atomic session state.
    if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire)) return;
    if (!ValidatePipboySession(context, record)) return;
    auto& p = g_pipboyScreen;
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    PipboyScreenShader role{};
    if (!ReadPipboyShaderRole(shader.Get(), role)) return;

    ObservePipboyShaderRole(context, p, role);
}

namespace {

[[nodiscard]] std::array<std::uint64_t, 36> LifecycleCounters(
    const fo4hdr::GraphicsObservationSnapshot& graphics) noexcept
{
    return {
        graphics.deviceCreateCount,
        graphics.deviceShadowCount,
        graphics.pixelShaderCreateCount,
        graphics.knownPixelShaderCreateCount,
        graphics.unknownPixelShaderCreateCount,
        graphics.abiExceptionCount,
        graphics.presentationProxyCreateCount,
        graphics.presentationRedirectCount,
        graphics.presentCount,
        graphics.hdrFrameCount,
        graphics.sdrFallbackFrameCount,
        graphics.heartbeatReadyFrameCount,
        graphics.heartbeatIncompleteFrameCount,
        graphics.heartbeatInvalidFrameCount,
        graphics.temporalAaDrawCount,
        graphics.temporalUnqualifiedDrawCount,
        graphics.temporalProxyDrawCount,
        graphics.temporalMissingProxyDrawCount,
        graphics.temporalAaFrameCount,
        graphics.temporalAaBypassFrameCount,
        graphics.temporalOutputRejectedCount,
        graphics.referenceRenderFailureCount,
        graphics.nativeUiBackdropCaptureDrawCount,
        graphics.nativeUiBackdropCaptureFrameCount,
        graphics.nativeUiCaptureDrawCount,
        graphics.nativeUiCaptureFrameCount,
        graphics.resourceGeneration,
        graphics.colorDomainGeneration,
        graphics.colorDomainMismatchCount,
        static_cast<std::uint64_t>(graphics.colorDomain),
        graphics.gameResetEpoch,
        graphics.performanceCpuMeasuredSamples,
        graphics.performanceGpuMeasuredSamples,
        graphics.qualifiedDrawThreadId,
        graphics.presentThreadId,
        (static_cast<std::uint64_t>(graphics.gameplayReady) << 3) |
            (static_cast<std::uint64_t>(graphics.hdrChainActive) << 2) |
            (static_cast<std::uint64_t>(graphics.hdrColorSpaceActive) << 1) |
            static_cast<std::uint64_t>(graphics.presentationPaused)
    };
}

[[nodiscard]] bool ShouldReportLifecycle(const fo4hdr::GraphicsObservationSnapshot& graphics) noexcept
{
    static std::array<std::atomic<std::uint64_t>, 36> lastCounters{};
    static std::atomic<bool> reported{};
    const auto currentCounters = LifecycleCounters(graphics);
    bool changed{};
    for (std::size_t index = 0; index < lastCounters.size(); ++index)
        changed |= lastCounters[index].exchange(currentCounters[index],
            std::memory_order_acq_rel) != currentCounters[index];
    return !reported.exchange(true) || changed;
}
[[nodiscard]] std::string FormatRendererDeviceStatus(
    const fo4hdr::GraphicsObservationSnapshot& graphics)
{
    return std::format(
        "m6 renderer compiled_in={} requested={} reference_requested={} initialized={} gameplay_ready={} "
        "hdr_active={} hdr_color_space={} heartbeat={} paused={} failure={} d3d_iat_owned={} d3d_iat_displaced={} "
        "device_calls={} device_next_calls={} device_successes={} device_shadows={} device_retires={} "
        "shadow_failures={} pixel_shader_calls={} known={} unknown={} known_mask={:#x} last_known_crc32={:#010x} "
        "device_hr={:#x} shader_hr={:#x} protection_restore_failures={} abi_exceptions={}",
        graphics.compiledIn,
        graphics.requested,
        graphics.developerReferenceRequested,
        graphics.initialized,
        graphics.gameplayReady,
        graphics.hdrChainActive,
        graphics.hdrColorSpaceActive,
        graphics.heartbeatReady,
        graphics.presentationPaused,
        fo4hdr::GraphicsObservationFailureName(graphics.failure),
        graphics.d3dIatOwned,
        graphics.d3dIatDisplaced,
        graphics.deviceCreateCount,
        graphics.deviceNextCallCount,
        graphics.successfulDeviceCreateCount,
        graphics.deviceShadowCount,
        graphics.deviceShadowRetireCount,
        graphics.deviceShadowInstallFailureCount,
        graphics.pixelShaderCreateCount,
        graphics.knownPixelShaderCreateCount,
        graphics.unknownPixelShaderCreateCount,
        graphics.knownShaderMask,
        graphics.lastKnownShaderCrc32,
        static_cast<std::uint32_t>(graphics.lastDeviceResult),
        static_cast<std::uint32_t>(graphics.lastPixelShaderResult),
        graphics.protectionRestoreFailureCount,
        graphics.abiExceptionCount);
}

[[nodiscard]] std::string FormatRendererFrameStatus(
    const fo4hdr::GraphicsObservationSnapshot& graphics)
{
    return std::format(
        " hdr_attempts={} fallbacks={} fallback_successes={} promoted={} backbuffer_proxies={} "
        "backbuffer_redirects={} presents={} resizes={} hdr_frames={} sdr_frames={} generation={} "
        "color_domain_generation={} color_domain_mismatches={} color_domain={} game_reset_epoch={} "
        "heartbeat_ready_frames={} heartbeat_incomplete_frames={} heartbeat_invalid_frames={} "
        "qualified_scene_frames={} taa_draws={} taa_unqualified_draws={} taa_proxy_draws={} "
        "taa_missing_proxy_draws={} taa_frames={} taa_bypass_frames={} taa_output_rejections={} "
        "reference_render_failures={} production_mode={}",
        graphics.hdrCreationAttemptCount,
        graphics.hdrCreationFallbackCount,
        graphics.hdrCreationFallbackSuccessCount,
        graphics.promotedResourceCount,
        graphics.presentationProxyCreateCount,
        graphics.presentationRedirectCount,
        graphics.presentCount,
        graphics.resizeCount,
        graphics.hdrFrameCount,
        graphics.sdrFallbackFrameCount,
        graphics.resourceGeneration,
        graphics.colorDomainGeneration,
        graphics.colorDomainMismatchCount,
        static_cast<std::uint32_t>(graphics.colorDomain),
        graphics.gameResetEpoch,
        graphics.heartbeatReadyFrameCount,
        graphics.heartbeatIncompleteFrameCount,
        graphics.heartbeatInvalidFrameCount,
        graphics.qualifiedSceneFrameCount,
        graphics.temporalAaDrawCount,
        graphics.temporalUnqualifiedDrawCount,
        graphics.temporalProxyDrawCount,
        graphics.temporalMissingProxyDrawCount,
        graphics.temporalAaFrameCount,
        graphics.temporalAaBypassFrameCount,
        graphics.temporalOutputRejectedCount,
        graphics.referenceRenderFailureCount,
        graphics.productionActive);
}

[[nodiscard]] std::string FormatRendererPerformanceStatus(
    const fo4hdr::GraphicsObservationSnapshot& graphics)
{
    return std::format(
        " production_requested={} production_operator_draws={} production_frames={} native_ui_backdrop_draws={} "
        "native_ui_backdrop_frames={} native_ui_draws={} native_ui_frames={} performance_requested={} "
        "performance_path={} performance_cpu_samples={} performance_gpu_samples={} performance_cpu_complete={} "
        "performance_gpu_complete={} performance_cpu_budget={} performance_gpu_budget={} performance_cpu_p95_ns={} "
        "performance_cpu_p99_ns={} performance_gpu_p95_ns={} performance_gpu_p99_ns={}",
        graphics.productionRequested,
        graphics.productionOperatorDrawCount,
        graphics.productionFrameCount,
        graphics.nativeUiBackdropCaptureDrawCount,
        graphics.nativeUiBackdropCaptureFrameCount,
        graphics.nativeUiCaptureDrawCount,
        graphics.nativeUiCaptureFrameCount,
        graphics.performanceRequested,
        graphics.performanceActivePath ? "active" : "disabled-boundary",
        graphics.performanceCpuMeasuredSamples,
        graphics.performanceGpuMeasuredSamples,
        graphics.performanceCpuComplete,
        graphics.performanceGpuComplete,
        graphics.performanceCpuBudgetPassed,
        graphics.performanceGpuBudgetPassed,
        graphics.performanceCpuP95Nanoseconds,
        graphics.performanceCpuP99Nanoseconds,
        graphics.performanceGpuP95Nanoseconds,
        graphics.performanceGpuP99Nanoseconds);
}

void ReportRendererStatus(const fo4hdr::GraphicsObservationSnapshot& graphics, GraphicsDiagnosticSink sink)
{
    sink(std::format("{}{}{}", FormatRendererDeviceStatus(graphics),
        FormatRendererFrameStatus(graphics), FormatRendererPerformanceStatus(graphics)));
    sink(std::format(
        "m14 live_settings thread_compatible={} "
        "qualified_draw_thread={} present_thread={}",
        graphics.liveSettingsThreadCompatible,
        graphics.qualifiedDrawThreadId,
        graphics.presentThreadId));

}

void ReportNewShaders(const fo4hdr::GraphicsObservationSnapshot& graphics, GraphicsDiagnosticSink sink)
{
    static std::atomic<std::uint32_t> reportedShaderMask{};
    const auto previousMask = reportedShaderMask.exchange(
        graphics.knownShaderMask,
        std::memory_order_acq_rel);
    const auto newlyObserved = graphics.knownShaderMask & ~previousMask;
    for (const auto& descriptor : fo4hdr::kRenoDxFallout4Shaders)
    {
        if ((newlyObserved & fo4hdr::RenoDxShaderBit(descriptor.pass)) != 0)
        {
            sink(std::format(
                "m6 known_shader pass={} crc32={:#010x} "
                "scene_input_slot={} domain=renodx-raw-dxbc-crc32",
                descriptor.name,
                descriptor.crc32,
                descriptor.sceneInputSlot));
        }
    }
}

} // namespace

void ReportGraphicsLifecycle(GraphicsDiagnosticSink sink) noexcept
{
    if (sink == nullptr) return;
    try
    {
        ReportFrameTrace(sink);
        const auto graphics = fo4hdr::ReadGraphicsObservation();
        if (!ShouldReportLifecycle(graphics)) return;
        ReportRendererStatus(graphics, sink);
        ReportNewShaders(graphics, sink);
    }
    catch (...)
    {
        ::OutputDebugStringA("FO4Isotope: M6 lifecycle diagnostics failed.\n");
    }
}

} // namespace fo4hdr
