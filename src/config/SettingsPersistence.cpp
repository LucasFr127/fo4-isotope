// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SettingsPersistence.hpp"
#include "FO4Isotope/config/SettingsPersistence.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"

#include <Windows.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fo4hdr {
namespace {

std::filesystem::path g_iniPath;
HdrSettingsProfiles g_requestedSettings{};
HdrInterfaceSettings g_requestedInterfaceSettings{};
std::atomic_flag g_requestLock = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> g_requestedGeneration{};
std::atomic<std::uint64_t> g_completedGeneration{};
std::atomic<SettingsPersistenceState> g_state{
    SettingsPersistenceState::unavailable};
std::atomic<bool> g_started{};
HANDLE g_saveEvent{};

void LockPersistenceWorker() noexcept
{
    while (g_requestLock.test_and_set(std::memory_order_acquire))
        YieldProcessor();
}

[[nodiscard]] std::filesystem::path ResolveBackingPath(
    const std::filesystem::path& path)
{
    const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return path;
    std::wstring buffer(32768U, L'\0');
    const auto length = GetFinalPathNameByHandleW(file, buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (length == 0 || length >= buffer.size()) return path;
    buffer.resize(length);
    constexpr std::wstring_view extendedPrefix{L"\\\\?\\"};
    constexpr std::wstring_view uncPrefix{L"\\\\?\\UNC\\"};
    if (buffer.starts_with(uncPrefix))
        buffer = L"\\\\" + buffer.substr(uncPrefix.size());
    else if (buffer.starts_with(extendedPrefix))
        buffer.erase(0, extendedPrefix.size());
    return std::filesystem::path(std::move(buffer));
}

// Keep reading and replacement in separate calls: Windows cannot replace the
// INI while std::ifstream owns its non-delete-sharing handle.
[[nodiscard]] bool ReadPersistenceFile(
    const std::filesystem::path& path,
    std::string& contents) noexcept
{
    try
    {
        // A deleted INI is rendered again from the saved settings.
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            if (error) return false;
            contents.clear();
            return true;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        contents.assign(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>{});
        return !input.bad();
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool ReplacePersistenceFile(
    const std::filesystem::path& path,
    std::string_view contents) noexcept
{
    try
    {
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(contents.data(),
                static_cast<std::streamsize>(contents.size()));
            output.flush();
            if (!output) return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            (void)DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool SaveSettings(
    const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    try
    {
        std::string original;
        if (!ReadPersistenceFile(g_iniPath, original)) return false;
        return ReplacePersistenceFile(g_iniPath,
            RewriteHdrSettingsIni(
                original, settings, interfaceSettings));
    }
    catch (...)
    {
        return false;
    }
}

struct PersistenceRequest
{
    HdrSettingsProfiles settings;
    HdrInterfaceSettings interfaceSettings;
    std::uint64_t generation{};
};

[[nodiscard]] PersistenceRequest ReadPersistenceRequest() noexcept
{
    LockPersistenceWorker();
    PersistenceRequest request;
    request.settings = g_requestedSettings;
    request.interfaceSettings = g_requestedInterfaceSettings;
    request.generation = g_requestedGeneration.load(std::memory_order_relaxed);
    g_requestLock.clear(std::memory_order_release);
    return request;
}

[[nodiscard]] std::uint64_t PublishPersistenceCompletion(
    const PersistenceRequest& request, bool saved) noexcept
{
    // Publication shares the request lock so an old completion cannot replace
    // a newer request's in-flight state.
    LockPersistenceWorker();
    g_completedGeneration.store(request.generation, std::memory_order_release);
    const auto latest = g_requestedGeneration.load(std::memory_order_acquire);
    auto state = SettingsPersistenceState::saving;
    if (request.generation == latest)
        state = saved ? SettingsPersistenceState::saved : SettingsPersistenceState::failed;
    g_state.store(state, std::memory_order_release);
    g_requestLock.clear(std::memory_order_release);
    return latest;
}

DWORD WINAPI PersistenceWorker(void*) noexcept
{
    std::uint64_t processed{};
    for (;;)
    {
        (void)WaitForSingleObject(g_saveEvent, INFINITE);
        const auto request = ReadPersistenceRequest();
        if (request.generation == processed) continue;
        processed = request.generation;
        const bool saved = SaveSettings(request.settings, request.interfaceSettings);
        if (PublishPersistenceCompletion(request, saved) != request.generation)
            (void)SetEvent(g_saveEvent);
    }
}

[[nodiscard]] bool InitializePersistencePaths(
    const std::filesystem::path& iniPath) noexcept
{
    try
    {
        const auto virtualMainPath = std::filesystem::absolute(iniPath);
        g_iniPath = ResolveBackingPath(virtualMainPath);
    }
    catch (...) { return false; }
    return true;
}

[[nodiscard]] bool TryLockPersistenceRequest() noexcept
{
    bool locked{};
    for (unsigned attempt = 0; attempt < 64 && !locked; ++attempt)
    {
        locked = !g_requestLock.test_and_set(std::memory_order_acquire);
        if (!locked) YieldProcessor();
    }
    return locked;
}

[[nodiscard]] bool ValidSettingsForPersistence(const HdrSettingsProfiles& settings) noexcept
{
    return ValidateHdrSettings(settings.hdr) == HdrSettingsValidationError::none &&
        ValidateProfileSettings(settings.sdr, true) == HdrSettingsValidationError::none;
}
}  // namespace

std::string RewriteHdrSettingsIni(
    std::string_view original,
    const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings)
{
    if (!ValidSettingsForPersistence(settings))
        return std::string(original);
    // The file keeps the keys the menu does not edit, such as DeveloperMode.
    auto parsed = ParsePluginConfiguration(original);
    // Only data of 1 GiB or more, or exhausted memory, fails to parse. Throwing makes
    // the worker report a failed save instead of writing the file back unchanged.
    if (!parsed.valid) throw std::runtime_error("FO4Isotope.ini could not be parsed for saving.");
    auto& stored = parsed.stored;
    static_cast<HdrSettings&>(stored) = settings.hdr;
    stored.sdrSettings = settings.sdr;
    stored.enabled = interfaceSettings.hdrEnabled;
    return RenderPluginConfiguration(stored);
}

bool WriteSettingsFile(
    const std::filesystem::path& iniPath, std::string_view text) noexcept
{
    try
    {
        return ReplacePersistenceFile(
            ResolveBackingPath(std::filesystem::absolute(iniPath)), text);
    }
    catch (...)
    {
        return false;
    }
}

bool InitializeSettingsPersistence(
    const std::filesystem::path& iniPath) noexcept
{
    if (g_started.load(std::memory_order_acquire))
        return g_iniPath == iniPath;
    if (!InitializePersistencePaths(iniPath)) return false;
    g_saveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_saveEvent == nullptr) return false;
    const auto thread = CreateThread(
        nullptr, 0, PersistenceWorker, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        CloseHandle(g_saveEvent);
        g_saveEvent = nullptr;
        return false;
    }
    CloseHandle(thread);
    g_state.store(SettingsPersistenceState::idle, std::memory_order_release);
    g_started.store(true, std::memory_order_release);
    return true;
}

std::uint64_t RequestHdrSettingsSave(
    const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    if (!g_started.load(std::memory_order_acquire) ||
        !ValidSettingsForPersistence(settings))
        return 0;
    if (!TryLockPersistenceRequest()) return 0;
    g_requestedSettings = settings;
    g_requestedInterfaceSettings = interfaceSettings;
    const auto generation =
        g_requestedGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_state.store(SettingsPersistenceState::saving, std::memory_order_release);
    g_requestLock.clear(std::memory_order_release);
    if (!SetEvent(g_saveEvent))
    {
        g_state.store(SettingsPersistenceState::failed,
            std::memory_order_release);
        return 0;
    }
    return generation;
}

SettingsPersistenceSnapshot ReadSettingsPersistenceSnapshot() noexcept
{
    return {
        .state = g_state.load(std::memory_order_acquire),
        .generation =
            g_completedGeneration.load(std::memory_order_acquire)};
}

void SettingsAutosave::Initialize(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) noexcept
{
    persistedSettings_ = observedSettings_ = settings;
    persistedInterface_ = observedInterface_ = interfaceSettings;
    changedAt_ = retryAt_ = requestGeneration_ = 0;
    requestFailed_ = false;
}

bool SettingsAutosave::Dirty(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings) const noexcept
{
    return settings != persistedSettings_ || interfaceSettings != persistedInterface_;
}

void SettingsAutosave::Acknowledge(const SettingsPersistenceSnapshot& persistence) noexcept
{
    if (requestGeneration_ == 0 || persistence.generation != requestGeneration_) return;
    if (persistence.state != SettingsPersistenceState::saved) return;
    persistedSettings_ = requestedSettings_;
    persistedInterface_ = requestedInterface_;
    requestGeneration_ = retryAt_ = 0;
    requestFailed_ = false;
}

bool SettingsAutosave::Ready(const SettingsPersistenceSnapshot& persistence,
    const SettingsAutosaveFrame& frame) const noexcept
{
    if (!frame.rendererReady || frame.now < retryAt_) return false;
    if (frame.windowOpen && frame.now - changedAt_ < 500U) return false;
    return persistence.state != SettingsPersistenceState::saving &&
        persistence.state != SettingsPersistenceState::unavailable;
}

void SettingsAutosave::Update(const HdrSettingsProfiles& settings,
    const HdrInterfaceSettings& interfaceSettings, const SettingsAutosaveFrame& frame) noexcept
{
    const auto persistence = ReadSettingsPersistenceSnapshot();
    Acknowledge(persistence);
    if (settings != observedSettings_ || interfaceSettings != observedInterface_)
    {
        observedSettings_ = settings;
        observedInterface_ = interfaceSettings;
        changedAt_ = frame.now;
    }
    if (!Dirty(settings, interfaceSettings) || !Ready(persistence, frame)) return;

    const auto generation = RequestHdrSettingsSave(settings, interfaceSettings);
    retryAt_ = frame.now + 2000U;
    requestFailed_ = generation == 0;
    if (generation == 0) return;
    requestedSettings_ = settings;
    requestedInterface_ = interfaceSettings;
    requestGeneration_ = generation;
}

}  // namespace fo4hdr
