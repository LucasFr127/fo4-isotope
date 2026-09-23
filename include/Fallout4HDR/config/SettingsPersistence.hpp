// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

#include "Fallout4HDR/config/HdrSettings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace fo4hdr {

enum class SettingsPersistenceState : std::uint8_t
{
    unavailable,
    idle,
    saving,
    saved,
    loading,
    missing,
    loaded,
    failed
};

inline constexpr std::size_t kMaximumDisplayIdentityLength{127};

struct DisplayCalibrationProfile
{
    std::array<wchar_t, kMaximumDisplayIdentityLength + 1> monitorDevicePath{};
    double displayPeakNits{};
    double uiPaperWhiteNits{};
    double videoWhiteNits{};
};

struct SettingsPersistenceSnapshot
{
    SettingsPersistenceState state{SettingsPersistenceState::unavailable};
    std::uint64_t generation{};
    SettingsPersistenceState displayCalibrationState{
        SettingsPersistenceState::unavailable};
    std::uint64_t displayCalibrationGeneration{};
    DisplayCalibrationProfile displayCalibration{};
};

[[nodiscard]] bool InitializeSettingsPersistence(
    const std::filesystem::path& iniPath,
    const LutGradeProfiles& distributedProfiles = {}) noexcept;
[[nodiscard]] std::uint64_t RequestHdrSettingsSave(
    const HdrSettings& settings) noexcept;
[[nodiscard]] SettingsPersistenceSnapshot
ReadSettingsPersistenceSnapshot() noexcept;
[[nodiscard]] std::uint64_t RequestDisplayCalibrationSave(
    std::wstring_view monitorDevicePath,
    const HdrSettings& settings) noexcept;
[[nodiscard]] std::uint64_t RequestDisplayCalibrationLoad(
    std::wstring_view monitorDevicePath) noexcept;

}  // namespace fo4hdr
