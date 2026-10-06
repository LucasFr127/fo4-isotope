// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "PluginPch.hpp"
#include "FO4Isotope/runtime/RuntimeValidation.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "../game/GameStateObserver.hpp"
#include "../game/ArmorUiObserver.hpp"
#include "../game/NativeLutObserver.hpp"
#include "../game/ImageSpaceObserver.hpp"
#include "../runtime/RuntimeIdentityFiles.hpp"

#include <F4SE/F4SE.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#if defined(FO4HDR_RUNTIME_1_10_163)
#  include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#endif

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

[[nodiscard]] constexpr std::string_view CompiledRuntime() noexcept
{
#if defined(FO4HDR_RUNTIME_1_10_163)
    return "1.10.163";
#elif defined(FO4HDR_RUNTIME_1_10_984)
    return "1.10.984";
#elif defined(FO4HDR_RUNTIME_1_11_221)
    return "1.11.221";
#else
#  error "No FO4Isotope runtime selected"
#endif
}

enum class PluginLoadState : std::uint8_t
{
    notStarted,
    initializing,
    active,
    failed
};

std::atomic<PluginLoadState> g_pluginLoadState{PluginLoadState::notStarted};

struct LegacyF4seInterface
{
    std::uint32_t f4seVersion;
    std::uint32_t runtimeVersion;
    std::uint32_t editorVersion;
    std::uint32_t isEditor;
    void*(F4SEAPI* queryInterface)(std::uint32_t);
    std::uint32_t(F4SEAPI* getPluginHandle)();
    std::uint32_t(F4SEAPI* getReleaseIndex)();
    const void*(F4SEAPI* getPluginInfo)(const char*);
};

static_assert(sizeof(LegacyF4seInterface) == 0x30);
static_assert(std::is_standard_layout_v<LegacyF4seInterface>);
static_assert(offsetof(LegacyF4seInterface, queryInterface) == 0x10);
static_assert(offsetof(LegacyF4seInterface, getPluginHandle) == 0x18);
static_assert(offsetof(LegacyF4seInterface, getReleaseIndex) == 0x20);
static_assert(offsetof(LegacyF4seInterface, getPluginInfo) == 0x28);

struct LegacyF4seServices
{
    const void* messaging{};
    const void* task{};
    const void* trampoline{};
    std::uint32_t pluginHandle{UINT32_MAX};
};

[[nodiscard]] constexpr REL::Version ExpectedF4seVersion() noexcept
{
    return { 0, 6, 23, 0 };
}

[[nodiscard]] bool IsExpectedLegacyRuntime(
    const F4SE::QueryInterface* f4se) noexcept
{
    return f4se != nullptr &&
           !f4se->IsEditor() &&
           f4se->F4SEVersion() == ExpectedF4seVersion() &&
           f4se->RuntimeVersion() == F4SE::RUNTIME_1_10_163;
}

void InitializeProjectLogging()
{
    wchar_t* knownBuffer{};
    const auto knownResult = REX::W32::SHGetKnownFolderPath(
        REX::W32::FOLDERID_Documents,
        REX::W32::KF_FLAG_DEFAULT,
        nullptr,
        &knownBuffer);
    std::unique_ptr<wchar_t[], decltype(&REX::W32::CoTaskMemFree)> knownPath(
        knownBuffer, REX::W32::CoTaskMemFree);
    if (!knownPath || knownResult != 0)
    {
        throw std::runtime_error("Documents folder is unavailable");
    }

    auto path = std::filesystem::path(knownPath.get()) /
        L"My Games" / L"Fallout4" / L"F4SE" / L"FO4Isotope.log";
    std::vector<spdlog::sink_ptr> sinks{
        std::make_shared<spdlog::sinks::msvc_sink_mt>(),
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            path.string(), true)
    };
    auto logger = std::make_shared<spdlog::logger>(
        "global", sinks.begin(), sinks.end());
#ifndef NDEBUG
    constexpr auto level = spdlog::level::debug;
#else
    constexpr auto level = spdlog::level::info;
#endif
    logger->set_level(level);
    logger->flush_on(level);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
    REX::INFO("FO4Isotope v0");
}

[[nodiscard]] LegacyF4seServices QueryLegacyF4seServices(
    const F4SE::LoadInterface* f4se)
{
    LegacyF4seInterface legacy{};
    std::memcpy(&legacy, f4se, sizeof(legacy));
    if (legacy.queryInterface == nullptr)
    {
        return {};
    }
    return {
        .messaging = legacy.queryInterface(F4SE::LoadInterface::kMessaging),
        .task = legacy.queryInterface(F4SE::LoadInterface::kTask),
        .trampoline = legacy.queryInterface(F4SE::LoadInterface::kTrampoline),
        .pluginHandle = legacy.getPluginHandle != nullptr ?
            legacy.getPluginHandle() : UINT32_MAX
    };
}

[[nodiscard]] std::optional<std::filesystem::path> ExecutablePath()
{
    std::array<wchar_t, 32768> buffer{};
    const auto size = REX::W32::GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<std::uint32_t>(buffer.size()));
    if (size == 0 || size >= buffer.size())
    {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data(), buffer.data() + size);
}

[[nodiscard]] std::optional<std::filesystem::path> LoadedModulePath(
    std::string_view moduleName)
{
    const std::string nullTerminatedName(moduleName);
    const auto module =
        REX::W32::GetModuleHandleA(nullTerminatedName.c_str());
    if (module == nullptr)
    {
        return std::nullopt;
    }

    std::array<wchar_t, 32768> buffer{};
    const auto size = REX::W32::GetModuleFileNameW(
        module,
        buffer.data(),
        static_cast<std::uint32_t>(buffer.size()));
    if (size == 0 || size >= buffer.size())
    {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data(), buffer.data() + size);
}

[[nodiscard]] std::string Version3(const REL::Version& version)
{
    return std::to_string(version.major()) + "." +
           std::to_string(version.minor()) + "." +
           std::to_string(version.patch());
}

[[nodiscard]] fo4hdr::RuntimeIdentity ExpectedIdentity() noexcept
{
#if defined(FO4HDR_RUNTIME_1_10_163)
    return {
        .runtimeVersion = fo4hdr::generated::kRuntimeVersion,
        .executableSha256 = fo4hdr::generated::kExecutableSha256,
        .f4seVersion = fo4hdr::generated::kF4seVersion,
        .f4seLoaderSha256 = fo4hdr::generated::kF4seLoaderSha256,
        .f4seRuntimeSha256 = fo4hdr::generated::kF4seRuntimeSha256,
        .addressLibraryFile = fo4hdr::generated::kAddressLibraryFile,
        .addressLibrarySha256 =
            fo4hdr::generated::kAddressLibrarySha256,
        .manifestSha256 = fo4hdr::generated::kManifestSha256
    };
#else
    return {
        .runtimeVersion = CompiledRuntime(),
        .executableSha256 = "UNVALIDATED",
        .f4seVersion = "UNVALIDATED",
        .f4seLoaderSha256 = "UNVALIDATED",
        .f4seRuntimeSha256 = "UNVALIDATED",
        .addressLibraryFile = "UNVALIDATED",
        .addressLibrarySha256 = "UNVALIDATED",
        .manifestSha256 = "UNVALIDATED"
    };
#endif
}

struct ObservedRuntimeIdentity
{
    std::string runtimeVersion;
    std::string executableHash;
    std::string f4seVersion;
    std::string loaderHash;
    std::string runtimeHash;
    std::string addressHash;
    std::string manifestHash;

    [[nodiscard]] fo4hdr::RuntimeIdentity View(std::string_view addressLibraryFile) const noexcept
    {
        return {
            .runtimeVersion = runtimeVersion,
            .executableSha256 = executableHash,
            .f4seVersion = f4seVersion,
            .f4seLoaderSha256 = loaderHash,
            .f4seRuntimeSha256 = runtimeHash,
            .addressLibraryFile = addressLibraryFile,
            .addressLibrarySha256 = addressHash,
            .manifestSha256 = manifestHash
        };
    }
};

[[nodiscard]] ObservedRuntimeIdentity ObserveIdentity(
    const F4SE::LoadInterface* f4se, const fo4hdr::RuntimeIdentity& expected)
{
    ObservedRuntimeIdentity observed;
    observed.runtimeVersion = f4se->RuntimeVersion().string();
    observed.f4seVersion = Version3(f4se->F4SEVersion());

    if (const auto executable = ExecutablePath())
    {
        const auto root = executable->parent_path();
        observed.executableHash = fo4hdr::Sha256File(*executable).value_or("");
#if defined(FO4HDR_RUNTIME_1_10_163)
        observed.loaderHash =
            fo4hdr::Sha256File(root / L"f4se_loader.exe").value_or("");
        if (const auto runtimeModule = LoadedModulePath(
                fo4hdr::generated::kF4seRuntimeFile))
        {
            observed.runtimeHash = fo4hdr::Sha256File(*runtimeModule).value_or("");
        }
        observed.addressHash = fo4hdr::Sha256File(
            root / L"Data" / L"F4SE" / L"Plugins" /
            std::filesystem::path(expected.addressLibraryFile))
                          .value_or("");
        observed.manifestHash = fo4hdr::Sha256CanonicalTextFile(
            root / L"Data" / L"F4SE" / L"Plugins" /
            L"FO4Isotope" / L"runtime-1.10.163.json")
                           .value_or("");
#endif
    }

    return observed;
}

[[nodiscard]] std::string_view DisplayHash(std::string_view hash) noexcept
{
    return hash.empty() ? "unavailable" : hash;
}

void ReportStartupException(const std::exception* error) noexcept
{
    try
    {
        if (error != nullptr)
        {
            REX::CRITICAL(
                "plugin active=false reason=startup-exception detail={}",
                error->what());
        }
        else
        {
            REX::CRITICAL("plugin active=false reason=startup-exception");
        }
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "FO4Isotope: startup exception; CommonLib logging unavailable.\n");
    }
}

void WriteGraphicsDiagnostic(std::string_view message)
{
    REX::INFO("{}", message);
}

void ReportLifecycleGraphics(const fo4hdr::GameHdrSnapshot&) noexcept
{
    fo4hdr::ReportGraphicsLifecycle(&WriteGraphicsDiagnostic);
}

}  // namespace

F4SE_EXPORT bool F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info) noexcept
{
    try
    {
        if (a_info == nullptr)
        {
            return false;
        }

        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = "FO4Isotope";
        a_info->version = 0;
        return IsExpectedLegacyRuntime(a_f4se);
    }
    catch (...)
    {
        return false;
    }
}

namespace
{

[[nodiscard]] bool ValidatePluginIdentity(const F4SE::LoadInterface* a_f4se)
{
    const auto expected = ExpectedIdentity();
    const auto identity = ObserveIdentity(a_f4se, expected);
    const auto observed = identity.View(expected.addressLibraryFile);

    REX::INFO("identity runtime={} executable_sha256={}",
        observed.runtimeVersion,
        DisplayHash(observed.executableSha256));
    REX::INFO("identity f4se={} loader_sha256={} runtime_dll_sha256={}",
        observed.f4seVersion,
        DisplayHash(observed.f4seLoaderSha256),
        DisplayHash(observed.f4seRuntimeSha256));
    REX::INFO("identity address_library={} sha256={}",
        observed.addressLibraryFile,
        DisplayHash(observed.addressLibrarySha256));
    REX::INFO("identity runtime_manifest_sha256={}",
        DisplayHash(observed.manifestSha256));
#if defined(FO4HDR_RUNTIME_1_10_163)
    REX::INFO(
        "identity commonlibf4_commit={}", fo4hdr::generated::kCommonLibCommit);
#else
    REX::INFO("identity commonlibf4_commit=unvalidated-profile");
#endif

    const auto validation = fo4hdr::ValidateRuntimeIdentity(expected, observed);
    if (!validation.valid)
    {
        REX::CRITICAL("plugin active=false reason={}",
            fo4hdr::ToString(validation.reason));
        return false;
    }

#if defined(FO4HDR_RUNTIME_1_10_163)
    constexpr auto runtimeAction = fo4hdr::RuntimeAction::MilestoneValidation;
    const auto actionAuthorization = fo4hdr::ValidateRuntimeAction(
        fo4hdr::generated::kRuntimeStatus, runtimeAction);
    const auto packagingAuthorization =
        fo4hdr::ValidateRuntimeAction(fo4hdr::generated::kRuntimeStatus,
            fo4hdr::RuntimeAction::ReleasePackaging);
    REX::INFO("identity runtime_status={} action={} authorized={} "
              "release_packaging_eligible={}",
        fo4hdr::generated::kRuntimeStatus,
        fo4hdr::ToString(runtimeAction),
        actionAuthorization.allowed,
        packagingAuthorization.allowed);
    if (!actionAuthorization.allowed)
    {
        REX::CRITICAL("plugin active=false reason={} runtime_status={}",
            fo4hdr::ToString(actionAuthorization.reason),
            fo4hdr::generated::kRuntimeStatus);
        return false;
    }
#endif
    REX::INFO("plugin active=true reason=exact-runtime-identity-matched");
    return true;
}

[[nodiscard]] fo4hdr::PluginConfigurationResult LoadDiagnosticsConfiguration(
    const std::optional<std::filesystem::path>& executable)
{
    std::optional<std::filesystem::path> settingsPath;
    auto diagnosticsConfiguration =
        fo4hdr::PluginConfigurationResult{};
    if (executable.has_value())
    {
        settingsPath = executable->parent_path() / L"Data" / L"F4SE" /
            L"Plugins" / L"FO4Isotope.ini";
        diagnosticsConfiguration =
            fo4hdr::LoadPluginConfiguration(
                *settingsPath);
        if (diagnosticsConfiguration.valid)
            (void)fo4hdr::InitializeSettingsPersistence(*settingsPath,
                diagnosticsConfiguration.distributedProfiles);
        for (const auto& issue : diagnosticsConfiguration.profileIssues)
        {
            const auto name = issue.path.u8string();
            REX::WARN("grade_profile_file ignored={} error={} line={}",
                std::string_view(reinterpret_cast<const char*>(name.data()),
                    name.size()), fo4hdr::ToString(issue.error), issue.line);
        }
    }
    REX::INFO(
        "m6 diagnostics_configuration valid={} error={} line={} "
        "developer_mode={} trace_render_pipeline={} measure_performance={} reference={} "
        "overlay={} forced_failure={} enabled={} production_requested={} "
        "algorithm={} display_peak_nits={} ui_paper_white_nits={} "
        "exposure_ev={} exposure_multiplier={}",
        diagnosticsConfiguration.valid,
        fo4hdr::ToString(diagnosticsConfiguration.error),
        diagnosticsConfiguration.line,
        diagnosticsConfiguration.value.developerMode,
        diagnosticsConfiguration.value.traceRenderPipeline,
        diagnosticsConfiguration.value.measurePerformance,
        diagnosticsConfiguration.value.developerHdr10Reference,
        diagnosticsConfiguration.value.showDeveloperOverlay,
        diagnosticsConfiguration.value.forceHdrSwapChainFailure,
        diagnosticsConfiguration.value.enabled,
        diagnosticsConfiguration.value.productionRequested,
        fo4hdr::ToString(diagnosticsConfiguration.value.productionAlgorithm),
        diagnosticsConfiguration.value.displayPeakNits,
        diagnosticsConfiguration.value.uiPaperWhiteNits,
        diagnosticsConfiguration.value.exposureEv,
        diagnosticsConfiguration.value.sceneExposureMultiplier);
    return diagnosticsConfiguration;
}

void ReportGameObserverStatus(bool observerActive) noexcept
{
    try
    {
        REX::INFO("m2 observer active={} reason={}",
            observerActive,
            observerActive ? "messaging-listener-registered"
                           : "messaging-unavailable");
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "FO4Isotope: M2 observer status logging failed.\n");
    }
}

struct ObserverBoundaries
{
    bool armorUi{};
    bool nativeLut{};
    bool imageSpace{};
};

[[nodiscard]] bool ProductionRequested(const fo4hdr::PluginConfigurationResult& configuration) noexcept
{
    return configuration.valid && configuration.value.enabled && configuration.value.productionRequested;
}

[[nodiscard]] ObserverBoundaries InitializeObserverBoundaries(bool observerActive,
    bool graphicsActive, const fo4hdr::PluginConfigurationResult& diagnosticsConfiguration,
    const LegacyF4seServices& legacyServices)
{
    ObserverBoundaries active;

#if defined(FO4HDR_RUNTIME_1_10_163)
    if (observerActive && graphicsActive)
    {
        active.nativeLut = fo4hdr::game::InitializeNativeLutObserver(
            ProductionRequested(diagnosticsConfiguration));
        active.imageSpace = fo4hdr::game::InitializeImageSpaceObserver(
            active.nativeLut, legacyServices.trampoline, legacyServices.pluginHandle);
        active.armorUi = fo4hdr::game::InitializeArmorUiObserver(
            ProductionRequested(diagnosticsConfiguration) ||
                (diagnosticsConfiguration.valid && diagnosticsConfiguration.value.developerMode &&
                    diagnosticsConfiguration.value.traceRenderPipeline),
            legacyServices.trampoline, legacyServices.pluginHandle);
    }
#endif
    return active;
}

void ReportInitializedRenderer(const ObserverBoundaries& boundaries,
    const fo4hdr::PluginConfigurationResult& diagnosticsConfiguration, bool graphicsActive) noexcept
{
    try
    {
        REX::INFO("armor UI composition boundary installed={}", boundaries.armorUi);
        REX::INFO("native LUT observation boundary installed={}", boundaries.nativeLut);
        REX::INFO("ImageSpace provenance boundaries installed={}", boundaries.imageSpace);
        REX::INFO(
            "hdr production_active=false production_requested={} "
            "algorithm={} display_peak_nits={} "
            "ui_paper_white_nits={} exposure_ev={} renderer_active={}",
            diagnosticsConfiguration.valid &&
                diagnosticsConfiguration.value.productionRequested,
            fo4hdr::ToString(
                diagnosticsConfiguration.value.productionAlgorithm),
            diagnosticsConfiguration.value.displayPeakNits,
            diagnosticsConfiguration.value.uiPaperWhiteNits,
            diagnosticsConfiguration.value.exposureEv,
            graphicsActive);
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "FO4Isotope: HDR inactive status logging failed.\n");
    }
}

[[nodiscard]] bool LoadPluginImpl(const F4SE::LoadInterface* a_f4se)
{
    if (!IsExpectedLegacyRuntime(a_f4se))
    {
        return false;
    }
    InitializeProjectLogging();
    const auto legacyServices = QueryLegacyF4seServices(a_f4se);

    if (!ValidatePluginIdentity(a_f4se)) return false;
#if defined(FO4HDR_RUNTIME_1_10_163)
    const auto executable = ExecutablePath();
    const auto diagnosticsConfiguration = LoadDiagnosticsConfiguration(executable);
    const auto observerActive =
        executable.has_value() &&
        fo4hdr::game::InitializeGameStateObserver(
            legacyServices.messaging,
            legacyServices.task,
            legacyServices.pluginHandle,
            &ReportLifecycleGraphics);
    ReportGameObserverStatus(observerActive);
#else
    REX::INFO("m2 observer active=false reason=runtime-profile-unvalidated");
    constexpr bool observerActive = false;
    const auto diagnosticsConfiguration =
        fo4hdr::PluginConfigurationResult{};
#endif
    const auto graphicsActive = fo4hdr::InitializeGraphicsObservation(
        diagnosticsConfiguration.valid ? diagnosticsConfiguration.value :
            fo4hdr::PluginConfiguration{});
    const auto boundaries = InitializeObserverBoundaries(observerActive, graphicsActive,
        diagnosticsConfiguration, legacyServices);
    ReportInitializedRenderer(boundaries, diagnosticsConfiguration, graphicsActive);
    // Listener registration and IAT installation publish process-lifetime
    // callbacks into F4SE/the executable. From the observer call onward,
    // every operation contains its own failures and load must succeed so
    // F4SE cannot unload code still referenced by those callbacks.
    return true;
}

}  // namespace

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se) noexcept
{
    auto expectedState = PluginLoadState::notStarted;
    if (!g_pluginLoadState.compare_exchange_strong(expectedState,
            PluginLoadState::initializing,
            std::memory_order_acq_rel))
    {
        return expectedState == PluginLoadState::active;
    }

    try
    {
        const auto loaded = LoadPluginImpl(a_f4se);
        g_pluginLoadState.store(
            loaded ? PluginLoadState::active : PluginLoadState::failed,
            std::memory_order_release);
        return loaded;
    }
    catch (const std::exception& error)
    {
        g_pluginLoadState.store(
            PluginLoadState::failed, std::memory_order_release);
        ReportStartupException(&error);
        return false;
    }
    catch (...)
    {
        g_pluginLoadState.store(
            PluginLoadState::failed, std::memory_order_release);
        ReportStartupException(nullptr);
        return false;
    }
}
