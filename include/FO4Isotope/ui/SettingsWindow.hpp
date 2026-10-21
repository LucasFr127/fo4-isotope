// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"
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

struct WeatherLutResponseView
{
    std::array<LutSnapshot, 4> currentLuts{};
    std::uint32_t validatedSlots{};
    const char* status{};
    std::array<ImageSpaceIdentity, 4> currentImageSpaces{};
    std::uint32_t imageSpaceValidSlots{};
    const char* imageSpaceStatus{};
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
struct SettingsWindowView
{
    HdrSettings activeSettings;
    bool lutPending{}, liveSettingsError{}, developerMode{}, hdrActive{};
    bool exposureRequested{}, exposureActive{}, exposureFailed{}, threadCompatible{};
    ProductionToneMapperAlgorithm pendingAlgorithm{};
    double pendingPeakNits{};
    const char* nativeReferenceStatus{};
    const char* armorEncodedStatus{"Off"};
    const char* rendererFailure{"none"};
    std::uint64_t resourceGeneration{}, frame{}, settingsGeneration{}, desiredSettingsGeneration{}, lutRequestGeneration{};
    // Settings-layer pixel size; it can differ from the game window's client size.
    std::uint32_t targetWidth{}, targetHeight{};
    SettingsDisplayView display;
    WeatherLutResponseView weatherLutResponse;
    SettingsPipboyView pipboy;
};

struct SettingsWindowActions
{
    // Required synchronous actions execute on the render thread.
    void (*focusLost)() noexcept;
    void (*requestExposure)(bool) noexcept;
    void (*requestPipboyObservation)(bool) noexcept;
    void (*refreshView)(SettingsWindowView&) noexcept;
};

struct SettingsInputActivation
{
    bool ready{}, active{};
    std::uint32_t currentThreadId{}, windowThreadId{};
};

void InitializeSettingsWindow(const HdrSettings& settings, const HdrInterfaceSettings& interfaceSettings) noexcept;
void EnsureSettingsWindowBackend(bool requested, HWND window, ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
void ShutdownSettingsWindowBackend() noexcept;
[[nodiscard]] bool SettingsWindowBackendReady() noexcept;
[[nodiscard]] SettingsInputActivation TryActivateSettingsWindowInput() noexcept;
[[nodiscard]] bool DrawSettingsWindow(SettingsWindowView& view, const SettingsWindowActions& actions) noexcept;
[[nodiscard]] bool SetSettingsWindowOpen(bool open) noexcept;
[[nodiscard]] bool SettingsWindowOpen() noexcept;
[[nodiscard]] const HdrSettings& ReadSettingsDraft() noexcept;
[[nodiscard]] bool SettingsCommitRequested() noexcept;
[[nodiscard]] bool ConsumeSettingsCommitRequest() noexcept;

} // namespace fo4hdr
