// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/runtime/RuntimeValidation.hpp"
#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "../game/GameStateObserver.hpp"
#include "../runtime/RuntimeIdentityFiles.hpp"
#include "../graphics/GameDeviceGeneration.hpp"

#include <F4SE/F4SE.h>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>

namespace {

constexpr char pluginName[] = "Fallout4HDR";
constexpr REL::Version pluginVersion{0, 1, 0, 0};
std::once_flag startup;
std::atomic<bool> graphicsInstallationAttempted{};

[[nodiscard]] fo4hdr::RuntimeVersion PlainVersion(const REL::Version& version) noexcept
{
    return {version[0], version[1], version[2], version[3]};
}

[[nodiscard]] bool ValidateStartup(const F4SE::QueryInterface* f4se) noexcept
{
    if (f4se == nullptr || f4se->IsEditor())
    {
        return false;
    }

    const fo4hdr::RuntimeIdentity reported{
        PlainVersion(f4se->RuntimeVersion()), PlainVersion(f4se->F4SEVersion())};
    if (!fo4hdr::ValidateRuntimeIdentity(reported).valid)
    {
        return false;
    }
    const auto executable = fo4hdr::ReadCurrentExecutableVersion();
    return executable && *executable == reported.runtimeVersion;
}

void ReadStartupSettings()
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        spdlog::error("Cannot resolve the settings path");
        return;
    }
    const std::filesystem::path executable(path.data(), path.data() + length);
    const auto configuration = fo4hdr::LoadPluginConfiguration(executable.parent_path() /
        L"Data/F4SE/Plugins/Fallout4HDR.ini");
    if (configuration.status == fo4hdr::PluginConfigurationStatus::missing)
    {
        spdlog::info("HDR settings file is absent; defaults apply");
        return;
    }
    if (configuration.status != fo4hdr::PluginConfigurationStatus::loaded)
    {
        spdlog::error("HDR settings could not be loaded: status={}, line={}",
            static_cast<unsigned>(configuration.status), configuration.line);
        return;
    }
    const auto& settings = configuration.value;
    spdlog::info("HDR settings: operator={}, peak={}, UI white={}, video white={}",
        fo4hdr::ToString(settings.algorithm), settings.display.displayPeakNits,
        settings.display.uiPaperWhiteNits, settings.display.videoWhiteNits);
}

void InstallStartupGraphics() noexcept
{
    // A failed protected write can leave the callback reachable; never reject this
    // resident module after attempting installation merely because diagnostics fail.
    graphicsInstallationAttempted.store(true, std::memory_order_release);
    const auto graphics = fo4hdr::InitializeGameDeviceGeneration();
    try
    {
        spdlog::info("Graphics device observation installation: status={}",
            static_cast<unsigned>(graphics));
    }
    catch (...)
    {}
}

}  // namespace

F4SE_PLUGIN_VERSION = []() constexpr {
    F4SE::PluginVersionData data{};
    data.PluginVersion(pluginVersion);
    data.PluginName(pluginName);
    data.HasNoStructUse(false);
    data.CompatibleVersions({REL::Version{1, 10, 163, 0}});
    data.MinimumRequiredXSEVersion(REL::Version{0, 6, 23, 0});
    return data;
}();

F4SE_EXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* f4se, F4SE::PluginInfo* info) noexcept
{
    try
    {
        if (info == nullptr)
        {
            return false;
        }
        info->infoVersion = F4SE::PluginInfo::kVersion;
        info->name = pluginName;
        info->version = pluginVersion.pack();
        return ValidateStartup(f4se);
    }
    catch (...)
    {
        return false;
    }
}

F4SE_EXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* f4se) noexcept
{
    try
    {
        if (!ValidateStartup(f4se))
        {
            return false;
        }
        std::call_once(startup, [] {
            ReadStartupSettings();
            spdlog::info("Fallout4HDR 0.1.0 loaded: runtime 1.10.163, F4SE 0.6.23");
            InstallStartupGraphics();
        });
        fo4hdr::game::InitializeGameStateObserver(f4se);
        return true;
    }
    catch (...)
    {
        return graphicsInstallationAttempted.load(std::memory_order_acquire);
    }
}
