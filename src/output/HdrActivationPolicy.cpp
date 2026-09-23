// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/HdrActivationPolicy.hpp"

namespace fo4hdr {
namespace {

[[nodiscard]] HdrActivationDecision Reject(
    HdrActivationReason reason) noexcept
{
    return {
        .mode = HdrActivationMode::vanillaSdr,
        .reason = reason,
        .hdrActive = false,
        .production = false
    };
}

[[nodiscard]] HdrActivationReason CheckHdrPath(
    const HdrPathCapabilities& capabilities) noexcept
{
    const struct Requirement
    {
        bool met;
        HdrActivationReason failure;
    } requirements[]{
        {capabilities.runtimeAuthorized, HdrActivationReason::runtimeUnauthorized},
        {capabilities.signalContractReady, HdrActivationReason::signalContractNotReady},
        {capabilities.fp16WorkingImage, HdrActivationReason::workingFormatUnavailable},
        {capabilities.r10g10b10a2Presentation, HdrActivationReason::presentationFormatUnavailable},
        {capabilities.flipModel, HdrActivationReason::flipModelRequired},
        {capabilities.bufferCount >= 2, HdrActivationReason::insufficientBufferCount},
        {capabilities.pqBt2020PresentSupported, HdrActivationReason::colorSpaceUnsupported},
        {capabilities.pqBt2020Applied, HdrActivationReason::colorSpaceApplicationFailed},
        {capabilities.resourcesReady, HdrActivationReason::resourcesUnavailable}
    };
    for (const auto& requirement : requirements)
    {
        if (!requirement.met) return requirement.failure;
    }
    return HdrActivationReason::allowed;
}

}  // namespace

HdrActivationDecision DecideHdrActivation(
    const HdrActivationRequest& request,
    const HdrPathCapabilities& capabilities) noexcept
{
    if (!request.enabled) return Reject(HdrActivationReason::disabled);
    const auto failure = CheckHdrPath(capabilities);
    if (failure != HdrActivationReason::allowed) return Reject(failure);

    if (request.productionRequested && !capabilities.productionToneMapperReady)
        return Reject(HdrActivationReason::productionToneMapperUnavailable);
    if (request.productionRequested)
    {
        return {
            .mode = HdrActivationMode::productionHdr10,
            .reason = HdrActivationReason::allowed,
            .hdrActive = true,
            .production = true
        };
    }

    if (!request.developerReferenceRequested) return Reject(HdrActivationReason::disabled);
    if (!request.developerMode) return Reject(HdrActivationReason::developerModeRequired);
    return {
        .mode = HdrActivationMode::developerReferenceHdr10,
        .reason = HdrActivationReason::allowed,
        .hdrActive = true,
        .production = false
    };
}

namespace {

[[nodiscard]] bool IsPresentingState(HdrRendererState state) noexcept
{
    return state == HdrRendererState::vanillaSdr || state == HdrRendererState::hdr10Ready;
}

[[nodiscard]] bool CanLoseDevice(HdrRendererState state) noexcept
{
    switch (state)
    {
    case HdrRendererState::deviceLost:
    case HdrRendererState::detached:
    case HdrRendererState::disabled:
        return false;
    default:
        return true;
    }
}

}  // namespace

void HdrRendererLifecycle::InvalidateResources() noexcept
{
    ++snapshot_.resourceGeneration;
    snapshot_.resourcesPublished = false;
    snapshot_.hdrActive = false;
}

bool HdrRendererLifecycle::BindDeviceAndSwapChain() noexcept
{
    if (snapshot_.state != HdrRendererState::detached &&
        snapshot_.state != HdrRendererState::deviceLost)
    {
        return false;
    }

    ++snapshot_.deviceGeneration;
    ++snapshot_.swapChainGeneration;
    InvalidateResources();
    snapshot_.state = HdrRendererState::vanillaSdr;
    return true;
}

bool HdrRendererLifecycle::PublishHdrResources() noexcept
{
    if (snapshot_.state != HdrRendererState::vanillaSdr)
        return false;
    ++snapshot_.resourceGeneration;
    snapshot_.resourcesPublished = true;
    snapshot_.hdrActive = true;
    snapshot_.state = HdrRendererState::hdr10Ready;
    return true;
}

bool HdrRendererLifecycle::PauseHdrPresentation() noexcept
{
    if (snapshot_.state != HdrRendererState::hdr10Ready ||
        !snapshot_.resourcesPublished)
        return false;
    snapshot_.hdrActive = false;
    return true;
}

bool HdrRendererLifecycle::ResumeHdrPresentation() noexcept
{
    if (snapshot_.state != HdrRendererState::hdr10Ready ||
        !snapshot_.resourcesPublished)
        return false;
    snapshot_.hdrActive = true;
    return true;
}

bool HdrRendererLifecycle::DeactivateToSdr() noexcept
{
    if (snapshot_.state == HdrRendererState::vanillaSdr) return true;
    if (snapshot_.state != HdrRendererState::hdr10Ready) return false;
    RetireTo(HdrRendererState::vanillaSdr);
    return true;
}

bool HdrRendererLifecycle::BeginResize() noexcept
{
    if (!IsPresentingState(snapshot_.state)) return false;
    RetireTo(HdrRendererState::resizePending);
    return true;
}

bool HdrRendererLifecycle::CompleteResize(
    bool resizeSucceeded,
    bool hdrResourcesReady) noexcept
{
    if (snapshot_.state != HdrRendererState::resizePending)
        return false;
    if (!resizeSucceeded)
    {
        // Resize failure does not itself prove that the D3D device was lost.
        // BeginResize already retired the old publication; remain in bounded
        // SDR until a later successful resize, or let the explicit removed/
        // reset HRESULT path call OnDeviceLost().
        snapshot_.state = HdrRendererState::vanillaSdr;
        return false;
    }

    ++snapshot_.swapChainGeneration;
    if (hdrResourcesReady)
    {
        ++snapshot_.resourceGeneration;
        snapshot_.resourcesPublished = true;
        snapshot_.hdrActive = true;
        snapshot_.state = HdrRendererState::hdr10Ready;
    }
    else
    {
        snapshot_.state = HdrRendererState::vanillaSdr;
    }
    return true;
}

void HdrRendererLifecycle::OnDisplayChange() noexcept
{
    ++snapshot_.displayGeneration;
    if (IsPresentingState(snapshot_.state))
    {
        RetireTo(HdrRendererState::vanillaSdr);
    }
}

bool HdrRendererLifecycle::OnGameReset(std::uint64_t resetEpoch) noexcept
{
    if (resetEpoch == 0 || resetEpoch <= snapshot_.gameResetEpoch)
        return false;

    snapshot_.gameResetEpoch = resetEpoch;
    if (IsPresentingState(snapshot_.state))
    {
        // A Fallout gameplay/load reset invalidates scene lineage and temporal
        // history, not the unchanged DXGI device, swap chain, or plugin-owned
        // presentation resources. Advance the semantic generation and pause;
        // actual D3D retirement remains owned by resize/display/device events.
        ++snapshot_.resourceGeneration;
        snapshot_.hdrActive = false;
    }
    return true;
}

void HdrRendererLifecycle::OnDeviceLost() noexcept
{
    if (!CanLoseDevice(snapshot_.state)) return;
    InvalidateResources();
    ++snapshot_.deviceGeneration;
    ++snapshot_.swapChainGeneration;
    snapshot_.state = HdrRendererState::deviceLost;
}

void HdrRendererLifecycle::Disable() noexcept
{
    if (snapshot_.state == HdrRendererState::disabled) return;
    RetireTo(HdrRendererState::disabled);
}

void HdrRendererLifecycle::Detach() noexcept
{
    if (snapshot_.state == HdrRendererState::detached) return;
    RetireTo(HdrRendererState::detached);
}

void HdrRendererLifecycle::RetireTo(HdrRendererState state) noexcept
{
    InvalidateResources();
    snapshot_.state = state;
}

namespace {

GraphicsLifecyclePublisher g_graphicsLifecyclePublisher;

}  // namespace

GraphicsLifecyclePublication GraphicsLifecyclePublisher::Publish(
    bool gameplayReady,
    bool advanceResetEpoch) noexcept
{
    auto observed = packed_.load(std::memory_order_relaxed);
    for (;;)
    {
        const auto current = Decode(observed);
        const auto nextEpoch = advanceResetEpoch &&
                current.resetEpoch < kMaximumResetEpoch ?
            current.resetEpoch + 1 : current.resetEpoch;
        const auto desired =
            (nextEpoch << 2) |
            (current.presentationBlocked ? kPresentationBlockedMask : 0) |
            (gameplayReady ? kReadyMask : 0);
        if (desired == observed)
            return current;
        if (packed_.compare_exchange_weak(
                observed,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed))
        {
            return Decode(desired);
        }
    }
}

GraphicsLifecyclePublication
GraphicsLifecyclePublisher::PublishPresentationBlocked(
    bool presentationBlocked) noexcept
{
    auto observed = packed_.load(std::memory_order_relaxed);
    for (;;)
    {
        const auto current = Decode(observed);
        const auto desired =
            (current.resetEpoch << 2) |
            (presentationBlocked ? kPresentationBlockedMask : 0) |
            (current.gameplayReady ? kReadyMask : 0);
        if (desired == observed) return current;
        if (packed_.compare_exchange_weak(
                observed,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed))
            return Decode(desired);
    }
}

GraphicsLifecyclePublication GraphicsLifecyclePublisher::Read() const noexcept
{
    return Decode(packed_.load(std::memory_order_acquire));
}

void PublishGraphicsLifecycle(
    bool gameplayReady,
    bool advanceResetEpoch) noexcept
{
    (void)g_graphicsLifecyclePublisher.Publish(
        gameplayReady,
        advanceResetEpoch);
}

void PublishGraphicsPresentationBlocked(bool presentationBlocked) noexcept
{
    (void)g_graphicsLifecyclePublisher.PublishPresentationBlocked(
        presentationBlocked);
}

GraphicsLifecyclePublication ReadGraphicsLifecycle() noexcept
{
    return g_graphicsLifecyclePublisher.Read();
}

}  // namespace fo4hdr
