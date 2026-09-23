// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Fallout4HDR/config/PluginConfiguration.hpp"
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
extern std::unique_ptr<OperatorLutBuildResult> g_activeCpuLut;
extern bool g_liveSettingsError;
extern std::atomic<DWORD> g_currentFrameQualifiedDrawThread;
extern std::atomic<bool> g_currentFrameQualifiedDrawThreadConflict;
extern std::atomic<DWORD> g_observedQualifiedDrawThread;
extern std::atomic<DWORD> g_observedPresentThread;
extern std::atomic<bool> g_liveSettingsThreadCompatible;

bool BuildOperatorSettings(const HdrSettings& values, OperatorConstants& settings) noexcept;
bool BuildOperatorSettings(OperatorConstants& settings) noexcept;
bool RequiresOperatorLut(std::uint32_t mode) noexcept;

void ObserveQualifiedSettingsDrawThread() noexcept;
bool HdrPathRequested() noexcept;
void InitializeLiveSettings(const PluginConfiguration&) noexcept;
OperatorLutBuildRequest DesiredOperatorLutBuild() noexcept;
void CommitLiveSettingsAtEndPresent(ID3D11Device*, ID3D11DeviceContext*) noexcept;

} // namespace fo4hdr
