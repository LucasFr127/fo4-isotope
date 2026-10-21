// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "PipboyScreenObservation.hpp"
#include "DrawObservation.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <Windows.h>

namespace fo4hdr {

struct AtomicEvidence
{
    std::atomic<bool> requested{};
    std::atomic<bool> referenceRequested{};
    std::atomic<bool> productionRequested{};
    std::atomic<bool> productionActive{};
    std::atomic<bool> initialized{};
    std::atomic<bool> hdrChainActive{};
    std::atomic<bool> hdrColorSpaceActive{};
    std::atomic<bool> heartbeatReady{};
    std::atomic<bool> gameplayReady{};
    std::atomic<bool> presentationPaused{};
    std::atomic<bool> performanceRequested{};
    std::atomic<bool> performanceActivePath{};
    std::atomic<bool> performanceCpuComplete{};
    std::atomic<bool> performanceGpuComplete{};
    std::atomic<bool> performanceCpuBudgetPassed{};
    std::atomic<bool> performanceGpuBudgetPassed{};
    std::atomic<bool> enbDetected{};
    std::atomic<std::uint32_t> failure{};
    std::atomic<std::uint64_t> deviceCreateCount{};
    std::atomic<std::uint64_t> deviceNextCallCount{};
    std::atomic<std::uint64_t> successfulDeviceCreateCount{};
    std::atomic<std::uint64_t> deviceShadowCount{};
    std::atomic<std::uint64_t> deviceShadowRetireCount{};
    std::atomic<std::uint64_t> deviceShadowInstallFailureCount{};
    std::atomic<std::uint64_t> pixelShaderCreateCount{};
    std::atomic<std::uint64_t> knownPixelShaderCreateCount{};
    std::atomic<std::uint64_t> unknownPixelShaderCreateCount{};
    std::atomic<std::uint64_t> protectionRestoreFailureCount{};
    std::atomic<std::uint64_t> abiExceptionCount{};
    std::atomic<std::uint64_t> hdrCreationAttemptCount{};
    std::atomic<std::uint64_t> hdrCreationFallbackCount{};
    std::atomic<std::uint64_t> hdrCreationFallbackSuccessCount{};
    std::atomic<std::uint64_t> promotedResourceCount{};
    std::atomic<std::uint64_t> presentationProxyCreateCount{};
    std::atomic<std::uint64_t> presentationRedirectCount{};
    std::atomic<std::uint64_t> presentCount{};
    std::atomic<std::uint64_t> resizeCount{};
    std::atomic<std::uint64_t> hdrFrameCount{};
    std::atomic<std::uint64_t> sdrFallbackFrameCount{};
    std::atomic<std::uint64_t> heartbeatReadyFrameCount{};
    std::atomic<std::uint64_t> heartbeatIncompleteFrameCount{};
    std::atomic<std::uint64_t> heartbeatInvalidFrameCount{};
    std::atomic<std::uint64_t> qualifiedSceneFrameCount{};
    std::atomic<std::uint64_t> temporalAaDrawCount{};
    std::atomic<std::uint64_t> temporalUnqualifiedDrawCount{};
    std::atomic<std::uint64_t> temporalProxyDrawCount{};
    std::atomic<std::uint64_t> temporalMissingProxyDrawCount{};
    std::atomic<std::uint64_t> temporalAaFrameCount{};
    std::atomic<std::uint64_t> temporalAaBypassFrameCount{};
    std::atomic<std::uint64_t> temporalOutputRejectedCount{};
    std::atomic<std::uint64_t> referenceRenderFailureCount{};
    std::atomic<std::uint64_t> productionOperatorDrawCount{};
    std::atomic<std::uint64_t> productionFrameCount{};
    std::atomic<std::uint64_t> performanceCpuMeasuredSamples{};
    std::atomic<std::uint64_t> performanceGpuMeasuredSamples{};
    std::atomic<std::uint64_t> performanceCpuP95Nanoseconds{};
    std::atomic<std::uint64_t> performanceCpuP99Nanoseconds{};
    std::atomic<std::uint64_t> performanceGpuP95Nanoseconds{};
    std::atomic<std::uint64_t> performanceGpuP99Nanoseconds{};
    std::atomic<std::uint64_t> gameResetEpoch{};
    std::atomic<std::uint64_t> resourceGeneration{};
    std::atomic<std::uint64_t> colorDomainGeneration{};
    std::atomic<std::uint64_t> colorDomainMismatchCount{};
    std::atomic<std::uint32_t> colorDomain{};
    std::atomic<std::uint32_t> knownShaderMask{};
    std::atomic<std::uint32_t> lastKnownShaderCrc32{};
    std::atomic<std::int32_t> lastDeviceResult{E_FAIL};
    std::atomic<std::int32_t> lastPixelShaderResult{E_FAIL};
};

extern AtomicEvidence g_evidence;
void RecordFailure(GraphicsObservationFailure value) noexcept;
void RecordAbiException() noexcept;

struct PipboyScreenViewObservation
{
    std::uint64_t frame{};
    D3D11_TEXTURE2D_DESC texture{};
    DXGI_FORMAT format{};
    UINT dimension{}, mip{}, mipCount{};
};
struct PipboyScreenResources
{
    PipboyScreenObservation state;
    Microsoft::WRL::ComPtr<IUnknown> producerIdentity;
    PipboyScreenViewObservation producerInput, producerOutput, consumerInput;
    std::array<std::uint64_t, 6> rejections{};
    std::uint64_t producerResourceChanges{};
    const char* status{"Off"};
};
extern PipboyScreenResources g_pipboyScreen;
extern std::atomic<std::uint64_t> g_pipboyRetiredContext;
extern std::atomic<std::uint64_t> g_pipboyWatchedContext;


void StopPipboyScreenObservation(const char*) noexcept;
void ObservePipboyScreenDraw(ID3D11DeviceContext*, const DrawObservation&) noexcept;

} // namespace fo4hdr
