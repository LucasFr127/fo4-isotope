// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include "FO4Isotope/output/NativeLutObservation.hpp"
#include "FO4Isotope/output/WeatherGradeResponse.hpp"
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <array>
#include <cstdint>

namespace fo4hdr {

enum class StatusSeverity : std::uint8_t
{
    info,
    warning,
    error
};

// A renderer status line; the menu shows it as Info, Warning or Error feedback.
struct StatusMessage
{
    const char* text{};
    StatusSeverity severity{};
};

struct WeatherLutResponseView
{
    std::array<LutSnapshot, 4> currentLuts{};
    std::uint32_t validatedSlots{};
    StatusMessage status{};
    std::array<ImageSpaceIdentity, 4> currentImageSpaces{};
    std::uint32_t imageSpaceValidSlots{};
    StatusMessage imageSpaceStatus{};
    std::array<std::uint32_t, 4> referencingContexts{};
    std::array<ResolvedLutGrade, 4> resolved{};
    std::uint32_t automaticSlots{};
    std::array<const char*, 4> fallbackReasons{};
    bool gradingBypassed{};
};

struct SettingsDisplayView
{
    bool known{};
    std::array<wchar_t, 128> monitorDevicePath{};
    std::array<wchar_t, 64> friendlyName{};
    std::array<wchar_t, 32> deviceName{};
    float peakNits{}, fullFrameNits{}, sdrWhiteNits{};
};

struct SettingsPipboyObservation
{
    bool requested{};
    std::uint64_t generation{}, contextGeneration{};
    std::uint64_t producerDraws{}, consumerDraws{}, pendingConsumers{};
    std::uint64_t matchedConsumers{}, unmatchedConsumers{}, rejectedDraws{};
    std::uint64_t lastProducerFrame{}, lastConsumerFrame{}, lastMatchedFrame{}, lastProducerAge{};
};

struct SettingsPipboyViewObservation
{
    std::uint64_t frame{};
    D3D11_TEXTURE2D_DESC texture{};
    DXGI_FORMAT format{};
    UINT dimension{}, mip{}, mipCount{};
};

struct SettingsPipboyView
{
    SettingsPipboyObservation state;
    SettingsPipboyViewObservation producerInput, producerOutput, consumerInput;
    std::array<std::uint64_t, 6> rejections{};
    std::uint64_t producerResourceChanges{};
    const char* status{"Off"};
};

// Borrowed strings and snapshots remain valid for the synchronous draw call.
struct SettingsMenuView
{
    HdrSettings activeSettings;
    bool lutPending{}, liveSettingsError{}, developerMode{}, hdrActive{}, sdrMode{};
    // [General] HdrEnabled as this game session started.
    bool hdrEnabledAtStart{};
    SdrModeReason sdrModeReason{};
    // SDR mode kept the menu after its replacement shaders failed to build.
    bool sdrControlsUnavailable{};
    // ENB is loaded, so SDR mode substitutes no shader and the controls have no effect.
    bool enbDetected{};
    bool exposureRequested{}, exposureActive{}, exposureFailed{}, threadCompatible{};
    // FO4Isotope presents, and the menu callback runs on that thread, so its
    // edits reach the end-of-Present commit without crossing threads. Without
    // both, only these two fields and developerMode are filled.
    bool presenting{}, drawOnPresentThread{};
    ProductionToneMapperAlgorithm pendingAlgorithm{};
    double pendingPeakNits{};
    const char* affineStatus{};
    const char* armorEncodedStatus{"Off"};
    const char* rendererFailure{"none"};
    std::uint64_t resourceGeneration{}, frame{}, settingsGeneration{}, desiredSettingsGeneration{}, lutRequestGeneration{};
    SettingsDisplayView display;
    WeatherLutResponseView weatherLutResponse;
    SettingsPipboyView pipboy;
};

// Renderer entry points the menu calls on the render thread.
struct SettingsMenuRenderer
{
    void (*refreshView)(SettingsMenuView&) noexcept;
    // True once the renderer applied this draft, or when it commits nothing.
    bool (*draftApplied)(const HdrSettings&) noexcept;
    void (*requestExposure)(bool) noexcept;
    void (*requestPipboyObservation)(bool) noexcept;
};

enum class SettingsMenuRegistration : std::uint8_t
{
    hostAbsent,
    hostRejected,
    registrationFailed,
    registered
};

[[nodiscard]] const char* SettingsMenuRegistrationName(SettingsMenuRegistration result) noexcept;

void InitializeSettingsMenu(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings, bool developerMode) noexcept;
// The renderer selects the profile its device generation renders;
// the draft then edits that profile and Read/Commit act on it.
void SelectSettingsMenuProfile(bool sdr) noexcept;
// Graphics publishes its entry points once, before the host can draw.
void ConnectSettingsMenuRenderer(const SettingsMenuRenderer& renderer) noexcept;
// Registers the DearModdingUI client on the F4SE main thread at kPostPostLoad.
// Without the host the plugin runs headless from the INI.
[[nodiscard]] SettingsMenuRegistration RegisterSettingsMenu() noexcept;
[[nodiscard]] const HdrSettings& ReadSettingsDraft() noexcept;
[[nodiscard]] bool SettingsCommitRequested() noexcept;
[[nodiscard]] bool ConsumeSettingsCommitRequest() noexcept;

} // namespace fo4hdr
