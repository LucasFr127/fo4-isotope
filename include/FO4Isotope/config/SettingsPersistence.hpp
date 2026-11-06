// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"

#include <cstdint>
#include <filesystem>

namespace fo4hdr {

enum class SettingsPersistenceState : std::uint8_t
{
    unavailable,
    idle,
    saving,
    saved,
    failed
};

struct SettingsPersistenceSnapshot
{
    SettingsPersistenceState state{SettingsPersistenceState::unavailable};
    std::uint64_t generation{};
    SettingsPersistenceState positionState{SettingsPersistenceState::unavailable};
};

struct SettingsAutosaveFrame
{
    std::uint64_t now{};
    bool rendererReady{}, windowOpen{};
};

class SettingsAutosave
{
public:
    void Initialize(const HdrSettingsProfiles& settings,
        const HdrInterfaceSettings& interfaceSettings) noexcept;
    void Update(const HdrSettingsProfiles& settings, const HdrInterfaceSettings& interfaceSettings,
        const SettingsAutosaveFrame& frame) noexcept;
    [[nodiscard]] bool Dirty(const HdrSettingsProfiles& settings,
        const HdrInterfaceSettings& interfaceSettings) const noexcept;
    [[nodiscard]] bool RequestFailed() const noexcept { return requestFailed_; }

private:
    void Acknowledge(const SettingsPersistenceSnapshot& persistence) noexcept;
    [[nodiscard]] bool Ready(const SettingsPersistenceSnapshot& persistence,
        const SettingsAutosaveFrame& frame) const noexcept;
    HdrSettingsProfiles persistedSettings_{}, observedSettings_{}, requestedSettings_{};
    HdrInterfaceSettings persistedInterface_{}, observedInterface_{}, requestedInterface_{};
    std::uint64_t changedAt_{}, retryAt_{}, requestGeneration_{};
    bool requestFailed_{};
};

[[nodiscard]] bool InitializeSettingsPersistence(
    const std::filesystem::path& iniPath) noexcept;
[[nodiscard]] std::uint64_t RequestHdrSettingsSave(
    const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept;
[[nodiscard]] SettingsPersistenceSnapshot
ReadSettingsPersistenceSnapshot() noexcept;
// Position-only autosave never persists unsaved image/interface controls.
[[nodiscard]] bool RequestHdrWindowPositionSave(double x, double y) noexcept;

}  // namespace fo4hdr
