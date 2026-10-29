// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "OperatorLutWorker.hpp"
#include "OperatorConstants.hpp"
#include <Windows.h>
#include <d3d11.h>
#include <atomic>
#include <memory>

namespace fo4hdr {

extern PluginConfiguration g_configuration;
extern HdrSettings g_activeSettings;
extern std::uint64_t g_settingsGeneration;
extern std::uint64_t g_desiredSettingsGeneration;
extern std::uint64_t g_lutRequestGeneration;
extern std::uint64_t g_deviceGeneration;
extern bool g_lutPending;
extern OperatorLutBuildRequest g_pendingLutRequest;
extern bool g_liveSettingsError;
extern std::atomic<DWORD> g_currentFrameQualifiedDrawThread;
extern std::atomic<bool> g_currentFrameQualifiedDrawThreadConflict;
extern std::atomic<DWORD> g_observedQualifiedDrawThread;
extern std::atomic<DWORD> g_observedPresentThread;
extern std::atomic<bool> g_liveSettingsThreadCompatible;

bool BuildOperatorSettings(const HdrSettings& values, OperatorConstants& settings) noexcept;
bool BuildOperatorSettings(OperatorConstants& settings) noexcept;
bool RequiresOperatorLut(std::uint32_t mode) noexcept;
// Each target's retained CPU table survives device loss for re-upload.
[[nodiscard]] std::unique_ptr<OperatorLutBuildResult>& RetainedCpuLut(OperatorLutTarget target) noexcept;
// The table target of the selected settings profile.
[[nodiscard]] OperatorLutTarget ActiveOperatorLutTarget() noexcept;

void ObserveQualifiedSettingsDrawThread() noexcept;
bool HdrPathRequested() noexcept;
void InitializeLiveSettings(const PluginConfiguration&) noexcept;
// Latches SDR mode and selects the live profile it renders.
void SetSdrModeActive(bool active) noexcept;
OperatorLutBuildRequest DesiredOperatorLutBuild() noexcept;
// Applies the settings draft; end Present calls it when a commit is requested.
void CommitUiDraft(ID3D11Device*, ID3D11DeviceContext*) noexcept;
void CommitLiveSettingsAtEndPresent(ID3D11Device*, ID3D11DeviceContext*) noexcept;

} // namespace fo4hdr
