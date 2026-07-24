// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/PluginConfiguration.hpp"

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
    busy
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

private:
    struct SaveRequest
    {
        PluginConfiguration value;
        std::uint64_t generation{};
    };
    void Run() noexcept;
    void Publish(std::uint64_t generation, bool saved) noexcept;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::optional<SaveRequest> pending_;
    SettingsPersistenceSnapshot snapshot_;
    bool stopping_{};
};

}  // namespace fo4hdr
