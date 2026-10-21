// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "PluginPch.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "../game/GameStateObserver.hpp"
#include "../game/ArmorUiObserver.hpp"
#include "../game/NativeLutObserver.hpp"
#include "../game/NativeUiResources.hpp"
#include "../game/ImageSpaceObserver.hpp"
#include "../game/DirectCallPatch.hpp"

#include <F4SE/F4SE.h>
#include <REX/FModule.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include "FO4Isotope/runtime/RuntimeProfile.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

enum class PluginLoadState : std::uint8_t
{
    notStarted,
    initializing,
    active,
    failed
};

std::atomic<PluginLoadState> g_pluginLoadState{PluginLoadState::notStarted};

struct F4seInterfacePrefix
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

static_assert(sizeof(F4seInterfacePrefix) == 0x30);
static_assert(std::is_standard_layout_v<F4seInterfacePrefix>);
static_assert(offsetof(F4seInterfacePrefix, queryInterface) == 0x10);
static_assert(offsetof(F4seInterfacePrefix, getPluginHandle) == 0x18);
static_assert(offsetof(F4seInterfacePrefix, getReleaseIndex) == 0x20);
static_assert(offsetof(F4seInterfacePrefix, getPluginInfo) == 0x28);

struct F4seServices
{
    const void* messaging{};
    const void* task{};
    const void* trampoline{};
    std::uint32_t pluginHandle{UINT32_MAX};
};

// The pinned spdlog opens narrow filenames through the ANSI code page, which
// cannot name every Documents path. MSVC streams open filesystem paths wide.
class WideFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    explicit WideFileSink(const std::filesystem::path& path) :
        file_(path, std::ios::binary | std::ios::trunc)
    {
        if (!file_) throw std::runtime_error("log file could not be opened");
    }

protected:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        file_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    }
    void flush_() override { file_.flush(); }

private:
    std::ofstream file_;
};

[[nodiscard]] std::filesystem::path ProjectLogPath()
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

    auto directory = std::filesystem::path(knownPath.get()) /
        L"My Games" / L"Fallout4" / L"F4SE";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / L"FO4Isotope.log";
}

// Logging precedes every load decision and never fails Load; without a file
// the debugger sink still receives each rejection reason.
void InitializeProjectLogging() noexcept
{
    try
    {
        std::vector<spdlog::sink_ptr> sinks{
            std::make_shared<spdlog::sinks::msvc_sink_mt>()};
        std::string fileError;
        try
        {
            sinks.push_back(std::make_shared<WideFileSink>(ProjectLogPath()));
        }
        catch (const std::exception& error)
        {
            fileError = error.what();
        }
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
        if (!fileError.empty())
        {
            REX::WARN("log file unavailable detail={}", fileError);
        }
    }
    catch (...)
    {
        ::OutputDebugStringA("FO4Isotope: logging unavailable.\n");
    }
}

[[nodiscard]] F4seServices QueryF4seServices(
    const F4SE::LoadInterface* f4se)
{
    F4seInterfacePrefix legacy{};
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

[[nodiscard]] std::string Version3(const REL::Version& version)
{
    return std::to_string(version.major()) + "." +
           std::to_string(version.minor()) + "." +
           std::to_string(version.patch());
}

[[nodiscard]] fo4hdr::RuntimeFamily ActiveRuntimeFamily() noexcept
{
    switch (REX::FModule::GetRuntimeIndex())
    {
    case REX::FModule::Runtime::kOG:
        return fo4hdr::RuntimeFamily::og;
    case REX::FModule::Runtime::kNG:
        return fo4hdr::RuntimeFamily::ng;
    default:
        return fo4hdr::RuntimeFamily::ae;
    }
}

// After admission the plugin stays loaded, so its state is reported as resident.
void ReportStartupException(const std::exception* error, bool resident) noexcept
{
    try
    {
        const auto state = resident ? "resident=true" : "active=false";
        if (error != nullptr)
        {
            REX::CRITICAL(
                "plugin {} reason=startup-exception detail={}",
                state, error->what());
        }
        else
        {
            REX::CRITICAL("plugin {} reason=startup-exception", state);
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

// Logs one renderer warning per process; logging failures are contained.
void WarnOnce(std::atomic<bool>& reported, const char* message) noexcept
{
    if (reported.exchange(true, std::memory_order_acq_rel)) return;
    try
    {
        REX::WARN("{}", message);
    }
    catch (...)
    {
        ::OutputDebugStringA("FO4Isotope: renderer warning logging failed.\n");
    }
}

// Names the otherwise silent cases that keep HDR off because of another mod:
// a higher import interposer, such as Community Shaders upscaling or frame
// generation, created the device itself, or ENB would post-process in SDR.
void ReportRendererCompatibility(const fo4hdr::GameHdrSnapshot& snapshot) noexcept
{
    static std::atomic<bool> bypassReported{};
    static std::atomic<bool> enbReported{};
    const auto graphics = fo4hdr::ReadGraphicsObservation();
    const bool engineDeviceReady =
        snapshot.rendererInitialized && snapshot.rendererDeviceIdentity != 0;
    if (fo4hdr::DeviceCreationBypassed(
            graphics.initialized, graphics.deviceCreateCount, engineDeviceReady))
    {
        WarnOnce(bypassReported, "renderer active=false reason=device-creation-bypassed "
                                 "hint=disable-community-shaders-upscaling-and-frame-generation");
    }
    if (graphics.enbDetected)
    {
        WarnOnce(enbReported, "renderer hdr_active=false reason=enb-detected "
                              "hint=remove-enb-to-enable-hdr");
    }
}

void ReportLifecycleGraphics(const fo4hdr::GameHdrSnapshot& snapshot) noexcept
{
    fo4hdr::ReportGraphicsLifecycle(&WriteGraphicsDiagnostic);
    ReportRendererCompatibility(snapshot);
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
        // Load owns the runtime decision so that every rejection is logged.
        return a_f4se != nullptr && !a_f4se->IsEditor();
    }
    catch (...)
    {
        return false;
    }
}

namespace
{

[[nodiscard]] bool SelectRuntime(const F4SE::QueryInterface* a_f4se)
{
    const auto runtime = a_f4se->RuntimeVersion();
    const auto f4se = a_f4se->F4SEVersion();
    REX::INFO("runtime observed executable={} f4se={} editor={}",
        runtime.string(), Version3(f4se), a_f4se->IsEditor());
    if (a_f4se->IsEditor())
    {
        REX::CRITICAL("plugin active=false reason=editor-runtime");
        return false;
    }

    // CommonLib reports and terminates on an unsupported executable, and later
    // on a missing or invalid Address Library, before any relocation resolves.
    if (!fo4hdr::InitializeRuntimeProfile(ActiveRuntimeFamily()))
    {
        REX::CRITICAL("plugin active=false reason=runtime-profile-conflict");
        return false;
    }
    const auto& profile = *fo4hdr::ActiveRuntimeProfile();
    REX::INFO("runtime profile={} compiled_runtime_manifest_sha256={} commonlibf4_commit={}",
        profile.key, profile.manifestSha256, profile.commonLibCommit);
    REX::INFO("plugin active=true reason=runtime-admitted");
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
            (void)fo4hdr::InitializeSettingsPersistence(*settingsPath);
    }
    REX::INFO(
        "m6 diagnostics_configuration valid={} error={} line={} "
        "developer_mode={} trace_render_pipeline={} measure_performance={} shared_ui={} reference={} "
        "overlay={} forced_failure={} enabled={} production_requested={} "
        "algorithm={} display_peak_nits={} ui_paper_white_nits={} "
        "exposure_ev={} exposure_multiplier={}",
        diagnosticsConfiguration.valid,
        fo4hdr::ToString(diagnosticsConfiguration.error),
        diagnosticsConfiguration.line,
        diagnosticsConfiguration.value.developerMode,
        diagnosticsConfiguration.value.traceRenderPipeline,
        diagnosticsConfiguration.value.measurePerformance,
        diagnosticsConfiguration.value.productionRequested,
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

[[nodiscard]] bool ProductionRequested(const fo4hdr::PluginConfigurationResult& configuration) noexcept
{
    return configuration.valid && configuration.value.enabled && configuration.value.productionRequested;
}

[[nodiscard]] std::string FormatBytes(const fo4hdr::game::direct_call_patch::SiteMismatch& mismatch)
{
    std::string text;
    for (std::size_t index = 0; index < mismatch.observedSize; ++index)
        text += std::format("{}{:02X}", index ? " " : "", mismatch.observed[index]);
    return text.empty() ? "unreadable" : text;
}

// Names the first rejected native site of a boundary that did not install.
bool ReportBoundary(std::string_view boundary, bool installed) noexcept
{
    const auto mismatch = fo4hdr::game::direct_call_patch::TakeSiteMismatch();
    try
    {
        const auto rva = mismatch.address -
            reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        if (installed || !mismatch.recorded)
        {
            REX::INFO("{} boundary installed={}", boundary, installed);
        }
        else if (mismatch.id != 0)
        {
            REX::WARN("{} boundary installed=false site_rva=0x{:X} address_library_id={} "
                      "mapped_rva=0x{:X}",
                boundary, rva, mismatch.id, mismatch.mappedRva);
        }
        else
        {
            REX::WARN("{} boundary installed=false site_rva=0x{:X} observed_bytes={}",
                boundary, rva, FormatBytes(mismatch));
        }
    }
    catch (...)
    {
        ::OutputDebugStringA("FO4Isotope: boundary status logging failed.\n");
    }
    return installed;
}

void InitializeObserverBoundaries(bool observerActive,
    bool graphicsActive, const fo4hdr::PluginConfigurationResult& diagnosticsConfiguration,
    const F4seServices& services)
{
    if (!observerActive || !graphicsActive)
    {
        try
        {
            REX::INFO("native boundaries installed=false observer_active={} graphics_active={}",
                observerActive, graphicsActive);
        }
        catch (...)
        {
            ::OutputDebugStringA("FO4Isotope: boundary status logging failed.\n");
        }
        return;
    }

    (void)fo4hdr::game::direct_call_patch::TakeSiteMismatch();
    const bool sharedUiRequested = ProductionRequested(diagnosticsConfiguration);
    const bool nativeLut = ReportBoundary("native LUT observation",
        fo4hdr::game::InitializeNativeLutObserver(sharedUiRequested));
    (void)ReportBoundary("ImageSpace provenance",
        fo4hdr::game::InitializeImageSpaceObserver(
            nativeLut, services.trampoline, services.pluginHandle));
    (void)ReportBoundary("armor UI composition",
        fo4hdr::game::InitializeArmorUiObserver(
            sharedUiRequested ||
                (diagnosticsConfiguration.valid && diagnosticsConfiguration.value.developerMode &&
                    diagnosticsConfiguration.value.traceRenderPipeline),
            services.trampoline, services.pluginHandle));
    (void)ReportBoundary("native pause resource allocation",
        fo4hdr::game::InitializeNativeUiResources(
            sharedUiRequested, services.trampoline, services.pluginHandle));
    (void)ReportBoundary("native screen-space UI",
        fo4hdr::game::InitializeScreenUiObserver(
            sharedUiRequested, services.trampoline, services.pluginHandle));
}

void ReportInitializedRenderer(
    const fo4hdr::PluginConfigurationResult& diagnosticsConfiguration, bool graphicsActive) noexcept
{
    try
    {
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

enum class StartupStage : std::uint8_t
{
    preload,
    load
};

[[nodiscard]] constexpr std::string_view ToString(StartupStage stage) noexcept
{
    return stage == StartupStage::preload ? "preload" : "load";
}

enum class StartupState : std::uint8_t
{
    notStarted,
    preparing,
    rejected,
    prepared
};

struct PreparedStartup
{
    std::optional<std::filesystem::path> executable;
    fo4hdr::PluginConfigurationResult configuration;
    bool graphicsActive{};
};

std::atomic<StartupState> g_startupState{StartupState::notStarted};
PreparedStartup g_startup;

[[nodiscard]] bool StartupPrepared() noexcept
{
    return g_startupState.load(std::memory_order_acquire) == StartupState::prepared;
}

// Worker threads and the IAT hook outlive every later failure, so each step
// after admission contains its own errors and the plugin stays loaded.
void PrepareAdmittedStartup() noexcept
{
    try
    {
        g_startup.executable = ExecutablePath();
        g_startup.configuration = LoadDiagnosticsConfiguration(g_startup.executable);
        g_startup.graphicsActive = fo4hdr::InitializeGraphicsObservation(
            g_startup.configuration.valid ? g_startup.configuration.value :
                fo4hdr::PluginConfiguration{});
        if (g_startup.graphicsActive) fo4hdr::SetFrameTraceAutoReportSink(&WriteGraphicsDiagnostic);
    }
    catch (...)
    {
        ::OutputDebugStringA("FO4Isotope: admitted startup step failed.\n");
    }
}

// The first F4SE entry point admits the runtime and installs the D3D11 import:
// Preload on NG/AE, so later Load-stage interposers chain above it; Load on OG.
// Rejection precedes every side effect, so that caller may still fail and unload.
[[nodiscard]] const PreparedStartup* PrepareStartup(
    const F4SE::QueryInterface* a_f4se, StartupStage stage)
{
    auto expected = StartupState::notStarted;
    if (!g_startupState.compare_exchange_strong(expected, StartupState::preparing,
            std::memory_order_acq_rel))
    {
        return expected == StartupState::prepared ? &g_startup : nullptr;
    }
    InitializeProjectLogging();
    REX::INFO("startup stage={}", ToString(stage));
    if (!SelectRuntime(a_f4se))
    {
        g_startupState.store(StartupState::rejected, std::memory_order_release);
        return nullptr;
    }
    PrepareAdmittedStartup();
    g_startupState.store(StartupState::prepared, std::memory_order_release);
    return &g_startup;
}

[[nodiscard]] bool LoadPluginImpl(const F4SE::LoadInterface* a_f4se)
{
    if (a_f4se == nullptr) return false;
    const auto* startup = PrepareStartup(a_f4se, StartupStage::load);
    if (startup == nullptr) return false;
    const auto services = QueryF4seServices(a_f4se);
    const auto observerActive =
        startup->executable.has_value() &&
        fo4hdr::game::InitializeGameStateObserver(
            services.messaging,
            services.task,
            services.pluginHandle,
            &ReportLifecycleGraphics);
    ReportGameObserverStatus(observerActive);
    InitializeObserverBoundaries(
        observerActive, startup->graphicsActive, startup->configuration, services);
    ReportInitializedRenderer(startup->configuration, startup->graphicsActive);
    // Listener registration and IAT installation publish process-lifetime
    // callbacks into F4SE/the executable. From admission onward, every
    // operation contains its own failures and load must succeed so F4SE
    // cannot unload code still referenced by those callbacks.
    return true;
}

}  // namespace

F4SE_PLUGIN_PRELOAD(const F4SE::PreLoadInterface* a_f4se) noexcept
{
    // F4SE unloads the plugin on false, which is safe only before admission.
    try
    {
        return a_f4se != nullptr && PrepareStartup(a_f4se, StartupStage::preload) != nullptr;
    }
    catch (const std::exception& error)
    {
        ReportStartupException(&error, StartupPrepared());
        return StartupPrepared();
    }
    catch (...)
    {
        ReportStartupException(nullptr, StartupPrepared());
        return StartupPrepared();
    }
}

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
        ReportStartupException(&error, StartupPrepared());
    }
    catch (...)
    {
        ReportStartupException(nullptr, StartupPrepared());
    }
    // An admitted startup has published callbacks, so F4SE must not unload it.
    const auto resident = StartupPrepared();
    g_pluginLoadState.store(
        resident ? PluginLoadState::active : PluginLoadState::failed,
        std::memory_order_release);
    return resident;
}
