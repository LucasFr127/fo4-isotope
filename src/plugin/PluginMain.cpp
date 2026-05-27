// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/runtime/RuntimeValidation.hpp"
#include "../runtime/RuntimeIdentityFiles.hpp"

#include <F4SE/F4SE.h>
#include <spdlog/spdlog.h>

#include <mutex>

namespace {

constexpr char pluginName[] = "Fallout4HDR";
constexpr REL::Version pluginVersion{0, 1, 0, 0};
std::once_flag startup;

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

}  // namespace

F4SE_PLUGIN_VERSION = []() constexpr {
    F4SE::PluginVersionData data{};
    data.PluginVersion(pluginVersion);
    data.PluginName(pluginName);
    data.HasNoStructUse(true);
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
            spdlog::info("Fallout4HDR 0.1.0 loaded: runtime 1.10.163, F4SE 0.6.23");
        });
        return true;
    }
    catch (...)
    {
        return false;
    }
}
