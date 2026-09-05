// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/PluginConfiguration.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace fo4hdr {

enum class SettingsPersistenceState : std::uint8_t
{
    unavailable,
    idle,
    saving,
    saved,
    failed,
    busy,
    loading,
    loaded,
    missing
};

inline constexpr std::size_t kMaximumDisplayIdentityLength{127};

struct DisplayCalibrationProfile
{
    std::array<wchar_t, kMaximumDisplayIdentityLength + 1> monitorDevicePath{};
    double displayPeakNits{};
    double uiPaperWhiteNits{};
    double videoWhiteNits{};
};

struct DisplayCalibrationSnapshot
{
    SettingsPersistenceState state{SettingsPersistenceState::unavailable};
    std::uint64_t requestedGeneration{};
    std::uint64_t completedGeneration{};
    DisplayCalibrationProfile profile;
};

struct SettingsPersistenceSnapshot
{
    SettingsPersistenceState state{SettingsPersistenceState::unavailable};
    std::uint64_t requestedGeneration{};
    std::uint64_t completedGeneration{};
};

[[nodiscard]] std::string RewritePluginConfigurationIni(
    std::string_view original, const PluginConfiguration& value);

// Initialize/destruction are cold and externally serialized against callers.
// Requests/snapshots share one lock with the worker. Destruction drains accepted
// pending work and joins; no caller may retain this owner during destruction.
class SettingsPersistence
{
public:
    SettingsPersistence() = default;
    ~SettingsPersistence() noexcept;
    SettingsPersistence(const SettingsPersistence&) = delete;
    SettingsPersistence& operator=(const SettingsPersistence&) = delete;

    [[nodiscard]] bool Initialize(const std::filesystem::path& path) noexcept;
    // One try-lock replaces the pending slot; tokens must be nonzero/increasing.
    [[nodiscard]] bool RequestSave(
        const PluginConfiguration& value, std::uint64_t generation) noexcept;
    [[nodiscard]] SettingsPersistenceSnapshot Snapshot() const noexcept;
    [[nodiscard]] std::uint64_t RequestDisplayCalibrationSave(
        const DisplayCalibrationProfile& profile) noexcept;
    [[nodiscard]] std::uint64_t RequestDisplayCalibrationLoad(
        const DisplayCalibrationProfile& identity) noexcept;
    [[nodiscard]] DisplayCalibrationSnapshot DisplayCalibrationStatus() const noexcept;

private:
    struct SaveRequest
    {
        PluginConfiguration value;
        std::uint64_t generation{};
    };
    struct ProfileRequest
    {
        DisplayCalibrationProfile profile;
        std::uint64_t generation{};
        bool load{};
    };
    std::uint64_t QueueProfile(const DisplayCalibrationProfile& profile, bool load) noexcept;
    void PublishProfile(const DisplayCalibrationSnapshot& result) noexcept;
    void RunProfile(const ProfileRequest& request) noexcept;
    void Run() noexcept;
    void Publish(std::uint64_t generation, bool saved) noexcept;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::optional<SaveRequest> pending_;
    SettingsPersistenceSnapshot snapshot_;
    std::optional<ProfileRequest> pendingProfile_;
    DisplayCalibrationSnapshot profileSnapshot_;
    bool stopping_{};
};

}  // namespace fo4hdr
