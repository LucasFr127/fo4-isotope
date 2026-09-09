// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LiveSettings.hpp"
#include "Fallout4HDR/ui/SettingsWindow.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace fo4hdr {

struct NativeSettingsHostConfiguration
{
    ID3D11Device* device{};
    ID3D11DeviceContext1* context{};
    PresentationPass* presentation{};
    PresentationScope scope{PresentationScope::unavailable};
    HWND window{};
    UINT width{};
    UINT height{};
    std::uint64_t generation{};
    std::filesystem::path persistencePath;
    PluginConfiguration initial;
};

// Construct on the admitted context/HWND owner before starting either worker.
// Native records retain this one owner even after partial startup failure.
// Destruction is only a cold, quiescent controlled operation after RetireCold;
// no native ABI return path may delete it or join its workers.
class NativeSettingsHost
{
public:
    NativeSettingsHost() noexcept = default;
    NativeSettingsHost(const NativeSettingsHost&) = delete;
    NativeSettingsHost& operator=(const NativeSettingsHost&) = delete;

    // Output already exists at its stable address and borrows Window().
    // The supplied result is the exact table used for its initial scene tuple.
    [[nodiscard]] bool InitializeCold(const NativeSettingsHostConfiguration& configuration,
        std::unique_ptr<OperatorLutBuildResult>& prepared) noexcept;
    [[nodiscard]] SettingsWindow* Window() noexcept { return &window_; }
    [[nodiscard]] bool CommitClosedFrame(PresentationScope scope) noexcept;
    [[nodiscard]] bool SuspendSurfaceForResize() noexcept;
    // The same-address output has completed cold reconstruction; no borrow is
    // dereferenced between SuspendSurfaceForResize and this qualified call.
    [[nodiscard]] bool ResumeAfterResize(UINT width, UINT height, PresentationScope scope) noexcept;
    // Cold reconstruction consumes the active tuple, never the startup defaults
    // or a pending requested table. The retained window address stays unchanged.
    [[nodiscard]] bool ReadActiveOutputConfiguration(UINT width, UINT height,
        ScenePassRenderer& scenes, PresentationConfiguration& output) noexcept;
    void Revoke() noexcept;
    [[nodiscard]] bool RetireCold() noexcept;

private:
    enum class State { cold, active, suspended, failed, retired };
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    enum class Publication { cold, ready, revoked };
    [[nodiscard]] static bool HasOutputOwners(const NativeSettingsHostConfiguration& configuration) noexcept;
    [[nodiscard]] static bool HasStartupSettings(const NativeSettingsHostConfiguration& configuration) noexcept;
    [[nodiscard]] bool QualifiedWindow(const NativeSettingsHostConfiguration& configuration) const noexcept;
    [[nodiscard]] bool ActivateInitialSurface() noexcept;
    [[nodiscard]] bool PrepareOwners(std::unique_ptr<OperatorLutBuildResult>& prepared) noexcept;
    [[nodiscard]] bool PublishSurface(UINT width, UINT height) noexcept;
    [[nodiscard]] bool CanResume(UINT width, UINT height) const noexcept;
    [[nodiscard]] bool QualifiedStartup(const NativeSettingsHostConfiguration& configuration) const noexcept;
    [[nodiscard]] bool PrepareWindow(UINT width, UINT height) noexcept;
    void FailStartup() noexcept;

    const DWORD ownerThread_{GetCurrentThreadId()};
    OperatorLutWorker worker_;
    SettingsPersistence persistence_;
    LiveSettings live_;
    SettingsWindow window_;
    NativeSettingsHostConfiguration configuration_;
    std::atomic<Publication> publication_{Publication::cold};
    State state_{State::cold};
};

}  // namespace fo4hdr
