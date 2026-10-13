// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"

#include <cstdint>
#include <string_view>

namespace fo4hdr {

enum class GraphicsObservationFailure : std::uint32_t
{
    none,
    imageUnavailable,
    importUnavailable,
    uncallableTarget,
    iatInstallFailed,
    selfReferentialNext,
    registryBusy,
    registryExhausted,
    protectionRestoreFailed,
    deviceShadowFailed,
    distinctInterfaceIdentity,
    abiException,
    descriptorUnavailable,
    hdrAttemptFailed,
    swapChainShadowFailed,
    contextShadowFailed,
    shaderCreationFailed,
    resourceCreationFailed,
    colorSpaceUnsupported,
    colorSpaceRejected,
    requiredHeartbeatMissing,
    temporalOutputUnavailable,
    referenceRenderFailed,
    colorDomainMismatch,
    deviceLost
};

struct GraphicsObservationSnapshot
{
    bool compiledIn{};
    bool requested{};
    bool initialized{};
    bool developerReferenceRequested{};
    bool productionRequested{};
    bool productionActive{};
    bool hdrChainActive{};
    bool hdrColorSpaceActive{};
    bool heartbeatReady{};
    bool gameplayReady{};
    std::uint64_t gameResetEpoch{};
    bool presentationPaused{};
    bool performanceRequested{};
    bool performanceActivePath{};
    bool performanceCpuComplete{};
    bool performanceGpuComplete{};
    bool performanceCpuBudgetPassed{};
    bool performanceGpuBudgetPassed{};
    bool d3dIatOwned{};
    bool d3dIatDisplaced{};
    bool liveSettingsThreadCompatible{};
    std::uint32_t qualifiedDrawThreadId{};
    std::uint32_t presentThreadId{};
    GraphicsObservationFailure failure{GraphicsObservationFailure::none};
    std::uint64_t deviceCreateCount{};
    std::uint64_t deviceNextCallCount{};
    std::uint64_t successfulDeviceCreateCount{};
    std::uint64_t deviceShadowCount{};
    std::uint64_t deviceShadowRetireCount{};
    std::uint64_t deviceShadowInstallFailureCount{};
    std::uint64_t pixelShaderCreateCount{};
    std::uint64_t knownPixelShaderCreateCount{};
    std::uint64_t unknownPixelShaderCreateCount{};
    std::uint64_t protectionRestoreFailureCount{};
    std::uint64_t abiExceptionCount{};
    std::uint64_t hdrCreationAttemptCount{};
    std::uint64_t hdrCreationFallbackCount{};
    std::uint64_t hdrCreationFallbackSuccessCount{};
    std::uint64_t promotedResourceCount{};
    std::uint64_t presentationProxyCreateCount{};
    std::uint64_t presentationRedirectCount{};
    std::uint64_t presentCount{};
    std::uint64_t resizeCount{};
    std::uint64_t hdrFrameCount{};
    std::uint64_t sdrFallbackFrameCount{};
    std::uint64_t heartbeatReadyFrameCount{};
    std::uint64_t heartbeatIncompleteFrameCount{};
    std::uint64_t heartbeatInvalidFrameCount{};
    std::uint64_t qualifiedSceneFrameCount{};
    std::uint64_t temporalAaDrawCount{};
    std::uint64_t temporalUnqualifiedDrawCount{};
    std::uint64_t temporalProxyDrawCount{};
    std::uint64_t temporalMissingProxyDrawCount{};
    std::uint64_t temporalAaFrameCount{};
    std::uint64_t temporalAaBypassFrameCount{};
    std::uint64_t temporalOutputRejectedCount{};
    std::uint64_t referenceRenderFailureCount{};
    std::uint64_t productionOperatorDrawCount{};
    std::uint64_t productionFrameCount{};
    std::uint64_t performanceCpuMeasuredSamples{};
    std::uint64_t performanceGpuMeasuredSamples{};
    std::uint64_t performanceCpuP95Nanoseconds{};
    std::uint64_t performanceCpuP99Nanoseconds{};
    std::uint64_t performanceGpuP95Nanoseconds{};
    std::uint64_t performanceGpuP99Nanoseconds{};
    std::uint64_t resourceGeneration{};
    std::uint64_t colorDomainGeneration{};
    std::uint64_t colorDomainMismatchCount{};
    LinearColorDomain colorDomain{LinearColorDomain::unknown};
    std::uint32_t knownShaderMask{};
    std::uint32_t lastKnownShaderCrc32{};
    std::int32_t lastDeviceResult{};
    std::int32_t lastPixelShaderResult{};
};

struct GraphicsSettingsInputActivation
{
    bool ready{};
    bool active{};
    std::uint32_t currentThreadId{};
    std::uint32_t windowThreadId{};
};

// Production output may be enabled without DeveloperMode; tracing and
// reference diagnostics still require it.
[[nodiscard]] bool InitializeGraphicsObservation(
    const PluginConfiguration& configuration) noexcept;
void SetSharedUiPauseResourcesReady(bool ready) noexcept;
void DisableSharedUiPauseResources() noexcept;
// Initial Win32 subclass installation must run on the exact HWND owner thread.
// F4SE lifecycle callbacks provide bounded, D3D-free retry opportunities after
// the render thread has published the initialized ImGui window. The input
// boundary still verifies the exact HWND owner before an initial install. A
// retained project-owned subclass may also be reactivated safely after a
// device generation changes.
[[nodiscard]] GraphicsSettingsInputActivation
TryActivateGraphicsSettingsInputOnCurrentThread() noexcept;
[[nodiscard]] GraphicsObservationSnapshot ReadGraphicsObservation() noexcept;
// Called from the existing lifecycle callback, never the presenting hot path.
// The sink consumes each message synchronously; no view is retained.
using GraphicsDiagnosticSink = void (*)(std::string_view message);
void ReportGraphicsLifecycle(GraphicsDiagnosticSink sink) noexcept;
[[nodiscard]] const char* GraphicsObservationFailureName(
    GraphicsObservationFailure failure) noexcept;

}  // namespace fo4hdr
