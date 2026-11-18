// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstdint>
#include <atomic>
#include <limits>

#include <CommunityShadersAPI.h>

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

// The import hook is installed and the engine already holds a device, yet no
// creation call reached the hook: a higher interposer created the device
// without calling lower hooks, so FO4Isotope stays inactive.
[[nodiscard]] bool DeviceCreationBypassed(bool importHookInstalled,
    std::uint64_t deviceCreateCount, bool engineDeviceReady) noexcept;

// Lower creation layers may bind to the first device they observe, such as
// Community Shaders' one-time bootstrap on OG. Creation therefore never makes
// an HDR candidate that its post-creation checks are already known to reject.
enum class HdrCandidateDecision : std::uint8_t
{
    attempt,
    enbDetected,
    displayNotHdr10,
    descriptorUnsupported
};

struct HdrCandidateInputs
{
    bool enbDetected{};
    bool displayHdr10{};
    bool facadeRequired{};
    bool facadeDescriptorSupported{};
};

[[nodiscard]] HdrCandidateDecision DecideHdrCandidate(
    const HdrCandidateInputs& inputs) noexcept;

// Why the owner chain runs SDR mode; none keeps vanilla or the HDR chain.
enum class SdrModeReason : std::uint8_t
{
    none,
    hdrOff,
    configurationDefaults,
    enbDetected,
    displayNotHdr10,
    descriptorUnsupported,
    hdrCandidateRejected,
    hdrInstallFailed,
    // Hosted presentation leaves the swap chain to Community Shaders; HDR is off.
    hostedPresentation,
    // Hosted HDR was requested and the host answered without starting it: it
    // presents SDR, or its HDR failed one of the latch's checks.
    hostPresentsSdr,
    // Hosted HDR was requested and no host answered the state query.
    hostNotFound,
    // Reported, never latched, with HDR requested: the host shows HDR in a LUT output
    // format FO4Isotope does not write, so hosted HDR cannot latch and SDR mode yields.
    hostFormatUnknown,
    // Reported, never latched: the host shows HDR on a chain latched to SDR mode,
    // which yields so the host's own tone map and LUT draw.
    hostPresentsHdr
};

// SDR mode runs on every owner chain without an HDR descriptor, so the settings
// window has a route to the screen whenever HDR is off or unavailable.
struct SdrModeInputs
{
    bool requested{};
    bool hdrPathRequested{};
    // Meaningful only when the HDR path was requested.
    HdrCandidateDecision hdrCandidate{HdrCandidateDecision::attempt};
    bool enbDetected{};
    bool hdrChainCreated{};
    // An HDR chain whose installation rolled back to the original descriptor.
    bool hdrInstallFailed{};
    // FO4Isotope.ini could not be read, so startup took the SDR-only defaults.
    bool configurationDefaults{};
    // [General] HostedPresentation, which never requests the HDR path.
    bool hostedPresentation{};
    // HdrEnabled under hosted presentation, and whether a host answered.
    bool hostedHdrRequested{};
    bool hostAnswered{};
    // Hosted HDR latched, so the chain runs the HDR profile instead of SDR mode.
    bool hostedHdr{};
};

[[nodiscard]] SdrModeReason DecideSdrModeReason(const SdrModeInputs& inputs) noexcept;

// What a hosting presenter reports about its HDR output, as Community Shaders'
// public header declares it. The caller sets size and zeroes the rest, so a host
// older than lutOutputEncoding leaves it at signed gamma 2.2, the format
// FO4Isotope writes.
using HostHdrOutputState = CS_HdrOutputState;
static_assert(sizeof(HostHdrOutputState) == 24U);

// The host's state query; a plugin that exports it can host presentation.
inline constexpr char kHostStateQueryExport[] = "CS_GetHdrOutputState";

// Trusts a host's answer, null when it gave none, only when it is complete and
// its paper white is usable.
[[nodiscard]] bool HostReportsHdr(const HostHdrOutputState* answer) noexcept;
// A complete answer in the LUT output format FO4Isotope writes, whatever the
// host shows now; the format is fixed for a host build.
[[nodiscard]] bool HostFormatSupported(const HostHdrOutputState* answer) noexcept;

// What a hosting presenter shows. Its HDR setting is live, so this can change
// every frame; the LUT output format is fixed for a host build.
enum class HostOutput : std::uint8_t
{
    sdr,
    // HDR in the LUT output format FO4Isotope writes.
    hdr,
    // HDR in another LUT output format, which only the host's own shaders write.
    hdrUnknownFormat
};

// HDR needs a complete answer, whatever its paper white; no answer means SDR.
// sceneEncoding is not checked: it describes the image the host's own LUT pass reads.
[[nodiscard]] HostOutput ClassifyHostOutput(const HostHdrOutputState* answer) noexcept;
// While the host shows HDR, a hosted SDR reason gives way to the reason SDR mode
// yields, which names an unknown format only when HDR was requested; ENB, an
// unusable INI and a hosted HDR latch keep theirs.
[[nodiscard]] SdrModeReason SdrModeReasonForHost(SdrModeReason latched, HostOutput host) noexcept;

// Hosted HDR runs on the host's HDR container: a chain that reports
// R8G8B8A8_UNORM while its back buffer is FP16.
struct HostedHdrInputs
{
    bool hostedHdrRequested{};
    bool enbDetected{};
    bool configurationDefaults{};
    bool hostReportsHdr{};
    bool hostFormatSupported{};
    bool chainRgba8Unorm{};
    bool backBufferFp16{};
};

// Hosted HDR is available for the device generation: requested with a usable
// INI and no ENB, in a supported host format, on the host's HDR container. The
// host keeps that container whatever its HDR setting, so the session follows it.
[[nodiscard]] bool HostedHdrAvailable(const HostedHdrInputs& inputs) noexcept;
// Hosted HDR starts when it is available and the host reports HDR now.
[[nodiscard]] bool DecideHostedHdr(const HostedHdrInputs& inputs) noexcept;

// Whether a hosted session changes mode at the end of Present.
enum class HostedSwitch : std::uint8_t
{
    hold,
    toHdr,
    toSdr
};

struct HostedSwitchInputs
{
    bool available{};
    bool hostedHdr{};
    // A complete HDR answer with a usable paper white, which hosted HDR needs.
    bool hostReportsHdr{};
    // The host shows SDR, which SDR mode needs.
    bool hostShowsSdr{};
    // The HDR operators have no native curve, so hosted HDR needs its retained table.
    bool hdrTableRetained{};
    // Qualified draws ran only on the Present thread, where the settings menu commits.
    bool threadCompatible{};
    // A requested settings commit belongs to the profile it was made in.
    bool commitPending{};
    // A failed build disabled substitution for the generation.
    bool failed{};
};

// An available session follows the host's HDR setting. Otherwise the mode holds,
// and a mismatch yields to the host's own shaders.
[[nodiscard]] HostedSwitch DecideHostedSwitch(const HostedSwitchInputs& inputs) noexcept;
[[nodiscard]] const char* SdrModeReasonName(SdrModeReason reason) noexcept;

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
