// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeSettingsHost.hpp"

namespace fo4hdr {

bool NativeSettingsHost::OnOwnerThread() const noexcept
{
    return GetCurrentThreadId() == ownerThread_;
}

bool NativeSettingsHost::HasOutputOwners(const NativeSettingsHostConfiguration& configuration) noexcept
{
    return configuration.device && configuration.context && configuration.presentation &&
        configuration.scope == PresentationScope::qualifiedExclusive;
}

bool NativeSettingsHost::HasStartupSettings(const NativeSettingsHostConfiguration& configuration) noexcept
{
    return configuration.generation != 0 && configuration.width != 0 && configuration.height != 0 &&
        configuration.persistencePath.is_absolute() && ValidatePluginConfiguration(configuration.initial);
}

bool NativeSettingsHost::QualifiedWindow(const NativeSettingsHostConfiguration& configuration) const noexcept
{
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(configuration.window, &process);
    if (process != GetCurrentProcessId() || thread != ownerThread_) return false;
    const auto* input = SettingsInputForGeneration(configuration.generation);
    return input && input->Matches(configuration.window, ownerThread_);
}

bool NativeSettingsHost::QualifiedStartup(const NativeSettingsHostConfiguration& configuration) const noexcept
{
    if (!OnOwnerThread() || state_ != State::cold) return false;
    if (publication_.load(std::memory_order_acquire) != Publication::cold) return false;
    if (!HasOutputOwners(configuration) || !HasStartupSettings(configuration)) return false;
    return QualifiedWindow(configuration);
}

void NativeSettingsHost::FailStartup() noexcept
{
    // Cold startup still owns the exclusive context; no output callback is published.
    (void)window_.Retire();
    (void)live_.Retire();
    publication_.store(Publication::revoked, std::memory_order_release);
    state_ = State::failed;
}

bool NativeSettingsHost::PrepareWindow(UINT width, UINT height) noexcept
{
    auto* input = SettingsInputForGeneration(configuration_.generation);
    if (!input) return false;
    return window_.Initialize(configuration_.device, configuration_.context,
        {configuration_.window, width, height, input,
            &live_, &persistence_, configuration_.presentation});
}

bool NativeSettingsHost::PrepareOwners(std::unique_ptr<OperatorLutBuildResult>& prepared) noexcept
{
    return persistence_.Initialize(configuration_.persistencePath) && worker_.Initialize() &&
        live_.Initialize(configuration_.initial, prepared,
            {worker_, *configuration_.presentation, persistence_}, configuration_.scope);
}

bool NativeSettingsHost::PublishSurface(UINT width, UINT height) noexcept
{
    if (!PrepareWindow(width, height)) return false;
    if (!SetSettingsInputHostConsumerReady(configuration_.generation, true))
    {
        (void)window_.Retire();
        return false;
    }
    configuration_.width = width;
    configuration_.height = height;
    state_ = State::active;
    return true;
}

bool NativeSettingsHost::ActivateInitialSurface() noexcept
{
    auto expected = Publication::cold;
    if (!publication_.compare_exchange_strong(expected, Publication::ready,
            std::memory_order_acq_rel)) return false;
    if (!PublishSurface(configuration_.width, configuration_.height)) return false;
    return publication_.load(std::memory_order_acquire) == Publication::ready;
}

bool NativeSettingsHost::InitializeCold(const NativeSettingsHostConfiguration& configuration,
    std::unique_ptr<OperatorLutBuildResult>& prepared) noexcept
{
    if (!QualifiedStartup(configuration)) return false;
    if (!SetSettingsInputHostConsumerReady(configuration.generation, false)) return false;
    try
    {
        configuration_ = configuration;
        state_ = State::failed;
        if (!PrepareOwners(prepared)) return false;
        if (!ActivateInitialSurface())
        {
            FailStartup();
            return false;
        }
        return true;
    }
    catch (...)
    {
        FailStartup();
        return false;
    }
}

bool NativeSettingsHost::CommitClosedFrame(PresentationScope scope) noexcept
{
    if (!OnOwnerThread() || state_ != State::active) return false;
    if (publication_.load(std::memory_order_acquire) != Publication::ready) return false;
    return live_.CommitAtClosedFrame(scope);
}

bool NativeSettingsHost::SuspendSurfaceForResize() noexcept
{
    if (!OnOwnerThread()) return false;
    if (state_ == State::failed || state_ == State::cold) return true;
    if (state_ == State::suspended) return true;
    if (state_ != State::active) return false;
    // A refusal leaves both readiness and the live window untouched.
    if (!SetSettingsInputHostConsumerReady(configuration_.generation, false)) return false;
    if (!window_.Retire()) return false;
    state_ = State::suspended;
    return true;
}

bool NativeSettingsHost::CanResume(UINT width, UINT height) const noexcept
{
    return OnOwnerThread() && state_ == State::suspended && width != 0 && height != 0 &&
        publication_.load(std::memory_order_acquire) == Publication::ready;
}

bool NativeSettingsHost::ResumeAfterResize(UINT width, UINT height, PresentationScope scope) noexcept
{
    if (!CanResume(width, height)) return false;
    if (!live_.ReapplyAfterResize(scope)) return false;
    return PublishSurface(width, height);
}

bool NativeSettingsHost::ReadActiveOutputConfiguration(UINT width, UINT height,
    ScenePassRenderer& scenes, PresentationConfiguration& output) noexcept
{
    if (!CanResume(width, height)) return false;
    const auto live = live_.Snapshot();
    if (!live.available) return false;
    const auto& display = live.active.display;
    output = {width, height, static_cast<float>(display.displayPeakNits), &scenes,
        {static_cast<float>(display.uiPaperWhiteNits), static_cast<float>(display.videoWhiteNits)},
        &window_};
    return true;
}

void NativeSettingsHost::Revoke() noexcept
{
    // Terminal failure stops adoption without destroying borrowed state.
    publication_.store(Publication::revoked, std::memory_order_release);
    RevokeSettingsInputHost();
}

bool NativeSettingsHost::RetireCold() noexcept
{
    if (!OnOwnerThread()) return false;
    if (state_ == State::retired) return true;
    if (state_ == State::active && !SuspendSurfaceForResize()) return false;
    if (!window_.Retire() || !live_.Retire()) return false;
    publication_.store(Publication::revoked, std::memory_order_release);
    state_ = State::retired;
    return true;
}

}  // namespace fo4hdr
