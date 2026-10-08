// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstddef>

#include "FO4Isotope/config/HdrSettings.hpp"

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
    SettingsPersistenceState positionState{SettingsPersistenceState::unavailable};
    SettingsPersistenceState displayCalibrationState{
        SettingsPersistenceState::unavailable};
    std::uint64_t displayCalibrationGeneration{};
    DisplayCalibrationProfile displayCalibration{};
};

[[nodiscard]] bool InitializeSettingsPersistence(
    const std::filesystem::path& iniPath) noexcept;
[[nodiscard]] std::uint64_t RequestHdrSettingsSave(
    const HdrSettings& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept;
[[nodiscard]] SettingsPersistenceSnapshot
ReadSettingsPersistenceSnapshot() noexcept;
// Position-only autosave never persists unsaved image/interface controls.
[[nodiscard]] bool RequestHdrWindowPositionSave(double x, double y) noexcept;
[[nodiscard]] std::uint64_t RequestDisplayCalibrationSave(
    std::wstring_view monitorDevicePath,
    const HdrSettings& settings) noexcept;
[[nodiscard]] std::uint64_t RequestDisplayCalibrationLoad(
    std::wstring_view monitorDevicePath) noexcept;

}  // namespace fo4hdr
