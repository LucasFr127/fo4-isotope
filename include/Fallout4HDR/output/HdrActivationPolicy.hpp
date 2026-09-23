// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <atomic>
#include <limits>

namespace fo4hdr {

enum class HdrActivationMode : std::uint8_t
{
    vanillaSdr,
    developerReferenceHdr10,
    productionHdr10
};

enum class HdrActivationReason : std::uint8_t
{
    disabled,
    allowed,
    runtimeUnauthorized,
    developerModeRequired,
    productionToneMapperUnavailable,
    signalContractNotReady,
    workingFormatUnavailable,
    presentationFormatUnavailable,
    flipModelRequired,
    insufficientBufferCount,
    colorSpaceUnsupported,
    colorSpaceApplicationFailed,
    resourcesUnavailable
};

struct HdrActivationRequest
{
    bool enabled{};
    bool developerMode{};
    bool developerReferenceRequested{};
    bool productionRequested{};
};

struct HdrPathCapabilities
{
    bool runtimeAuthorized{};
    bool signalContractReady{};
    bool fp16WorkingImage{};
    bool r10g10b10a2Presentation{};
    bool flipModel{};
    std::uint32_t bufferCount{};
    bool pqBt2020PresentSupported{};
    bool pqBt2020Applied{};
    bool resourcesReady{};
    bool productionToneMapperReady{};
};

struct HdrActivationDecision
{
    HdrActivationMode mode{HdrActivationMode::vanillaSdr};
    HdrActivationReason reason{HdrActivationReason::disabled};
    bool hdrActive{};
    bool production{};
};

[[nodiscard]] HdrActivationDecision DecideHdrActivation(
    const HdrActivationRequest& request,
    const HdrPathCapabilities& capabilities) noexcept;

enum class HdrRendererState : std::uint8_t
{
    detached,
    vanillaSdr,
    hdr10Ready,
    resizePending,
    deviceLost,
    disabled
};

struct HdrRendererSnapshot
{
    HdrRendererState state{HdrRendererState::detached};
    std::uint64_t deviceGeneration{};
    std::uint64_t swapChainGeneration{};
    std::uint64_t resourceGeneration{};
    std::uint64_t displayGeneration{};
    std::uint64_t gameResetEpoch{};
    bool resourcesPublished{};
    bool hdrActive{};
};

class HdrRendererLifecycle
{
public:
    [[nodiscard]] bool BindDeviceAndSwapChain() noexcept;
    [[nodiscard]] bool PublishHdrResources() noexcept;
    [[nodiscard]] bool PauseHdrPresentation() noexcept;
    [[nodiscard]] bool ResumeHdrPresentation() noexcept;
    [[nodiscard]] bool DeactivateToSdr() noexcept;
    [[nodiscard]] bool BeginResize() noexcept;
    [[nodiscard]] bool CompleteResize(
        bool resizeSucceeded,
        bool hdrResourcesReady) noexcept;
    void OnDisplayChange() noexcept;
    [[nodiscard]] bool OnGameReset(std::uint64_t resetEpoch) noexcept;
    void OnDeviceLost() noexcept;
    void Disable() noexcept;
    void Detach() noexcept;

    [[nodiscard]] const HdrRendererSnapshot& Snapshot() const noexcept
    {
        return snapshot_;
    }

    [[nodiscard]] bool IsCurrentResourceGeneration(
        std::uint64_t generation) const noexcept
    {
        return generation != 0 && snapshot_.resourcesPublished &&
               generation == snapshot_.resourceGeneration;
    }

private:
    void InvalidateResources() noexcept;
    void RetireTo(HdrRendererState state) noexcept;
    HdrRendererSnapshot snapshot_{};
};

static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "Windows x64 lifecycle publication must remain lock-free");

// Plain game-thread publication consumed at the render-thread Present boundary.
// The packed representation makes readiness and its reset epoch one coherent
// acquire/release observation without publishing any game-owned state.
struct GraphicsLifecyclePublication
{
    bool gameplayReady{};
    bool presentationBlocked{};
    std::uint64_t resetEpoch{};
};

class GraphicsLifecyclePublisher
{
public:
    [[nodiscard]] GraphicsLifecyclePublication Publish(
        bool gameplayReady,
        bool advanceResetEpoch) noexcept;
    [[nodiscard]] GraphicsLifecyclePublication PublishPresentationBlocked(
        bool presentationBlocked) noexcept;
    [[nodiscard]] GraphicsLifecyclePublication Read() const noexcept;

private:
    static constexpr std::uint64_t kReadyMask{1};
    static constexpr std::uint64_t kPresentationBlockedMask{2};
    static constexpr std::uint64_t kMaximumResetEpoch{
        std::numeric_limits<std::uint64_t>::max() >> 2};

    [[nodiscard]] static constexpr GraphicsLifecyclePublication Decode(
        std::uint64_t packed) noexcept
    {
        return {
            .gameplayReady = (packed & kReadyMask) != 0,
            .presentationBlocked =
                (packed & kPresentationBlockedMask) != 0,
            .resetEpoch = packed >> 2
        };
    }

    std::atomic<std::uint64_t> packed_{};
};

void PublishGraphicsLifecycle(
    bool gameplayReady,
    bool advanceResetEpoch) noexcept;
void PublishGraphicsPresentationBlocked(bool presentationBlocked) noexcept;
[[nodiscard]] GraphicsLifecyclePublication ReadGraphicsLifecycle() noexcept;

}  // namespace fo4hdr
