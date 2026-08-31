// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "OperatorLutWorker.hpp"
#include "passes/PresentationPass.hpp"
#include "Fallout4HDR/config/PluginConfiguration.hpp"
#include "Fallout4HDR/config/SettingsPersistence.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace fo4hdr {

enum class LiveSettingsRequestStatus : std::uint8_t
{
    accepted,
    busy,
    invalid,
    failed,
    unavailable
};

enum class LiveSettingsError : std::uint8_t
{
    none,
    invalidConfiguration,
    workerBusy,
    workerFailed,
    applyRejected,
    generationExhausted,
    unavailable
};

struct LiveSettingsSnapshot
{
    PluginConfiguration active;
    PluginConfiguration requested;
    std::uint64_t activeGeneration{};
    std::uint64_t requestedGeneration{};
    bool available{};
    bool pending{};
    LiveSettingsError error{LiveSettingsError::unavailable};
    SettingsPersistenceSnapshot persistence;
    bool saveRejected{};
};

struct LiveSettingsOwners
{
    OperatorLutWorker& worker;
    PresentationPass& presentation;
    SettingsPersistence& persistence;
};

// Construct, initialize and mutate on one owner thread. Borrowed owners outlive
// this owner; the worker is exclusively submitted/consumed here after attachment.
// The initial generation dominates prior worker submissions. Retire before the
// borrowed owners; operator/peak changes require caller-owned history requalification.
class LiveSettings
{
public:
    LiveSettings() = default;
    ~LiveSettings() noexcept { (void)Retire(); }
    LiveSettings(const LiveSettings&) = delete;
    LiveSettings& operator=(const LiveSettings&) = delete;

    [[nodiscard]] bool Initialize(const PluginConfiguration& initial,
        std::unique_ptr<OperatorLutBuildResult>& prepared,
        LiveSettingsOwners owners, PresentationScope scope) noexcept
    {
        if (!OnOwnerThread()) return false;
        if (available_) return false;
        if (!ValidatePluginConfiguration(initial)) return false;
        if (!prepared) return false;
        if (!InitialResultMatches(initial, *prepared)) return false;
        const auto save = owners.persistence.Snapshot();
        if (save.state == SettingsPersistenceState::busy) return false;
        if (!owners.presentation.ApplySettings(
                prepared->data, Whites(initial), scope, Grade(initial))) return false;
        worker_ = &owners.worker;
        presentation_ = &owners.presentation;
        persistence_ = &owners.persistence;
        active_ = requested_ = initial;
        activeGeneration_ = requestedGeneration_ = prepared->request.requestGeneration;
        saveGeneration_ = save.requestedGeneration;
        activeTable_ = std::move(prepared);
        error_ = LiveSettingsError::none;
        saveRejected_ = false;
        available_ = true;
        return true;
    }

    [[nodiscard]] LiveSettingsRequestStatus Request(const PluginConfiguration& value) noexcept
    {
        if (!OnOwnerThread()) return LiveSettingsRequestStatus::unavailable;
        if (!available_) return LiveSettingsRequestStatus::unavailable;
        if (!ValidatePluginConfiguration(value))
        {
            error_ = LiveSettingsError::invalidConfiguration;
            return LiveSettingsRequestStatus::invalid;
        }
        if (KeepRequest(value)) return LiveSettingsRequestStatus::accepted;
        if (requestedGeneration_ == std::numeric_limits<std::uint64_t>::max())
        {
            error_ = LiveSettingsError::generationExhausted;
            return LiveSettingsRequestStatus::failed;
        }
        const auto generation = requestedGeneration_ + 1;
        const auto status = PrepareRequest(value, generation);
        if (status != LiveSettingsRequestStatus::accepted) return status;
        requested_ = value;
        requestedGeneration_ = generation;
        readyTable_.reset();
        pending_ = true;
        error_ = LiveSettingsError::none;
        return LiveSettingsRequestStatus::accepted;
    }

    [[nodiscard]] bool CommitAtClosedFrame(PresentationScope scope) noexcept
    {
        if (!OnOwnerThread()) return false;
        if (!available_) return false;
        if (!pending_) return false;
        if (!ApplyRequested(scope)) return false;
        if (readyTable_) activeTable_ = std::move(readyTable_);
        active_ = requested_;
        pending_ = false;
        error_ = LiveSettingsError::none;
        activeGeneration_ = requestedGeneration_;
        return true;
    }

    // Saving the accepted request is independent of native activation. Save
    // tokens do not describe render generations and never gate a native commit.
    [[nodiscard]] bool RequestSave() noexcept
    {
        if (!OnOwnerThread()) return false;
        if (!available_) return false;
        if (saveGeneration_ == std::numeric_limits<std::uint64_t>::max())
        {
            saveRejected_ = true;
            return false;
        }
        const auto generation = saveGeneration_ + 1;
        saveRejected_ = !persistence_->RequestSave(requested_, generation);
        if (!saveRejected_) saveGeneration_ = generation;
        return !saveRejected_;
    }

    [[nodiscard]] LiveSettingsSnapshot Snapshot() const noexcept
    {
        // The immutable thread identity is the only field read by other threads.
        if (!OnOwnerThread()) return {};
        if (!available_) return {};
        return {active_, requested_, activeGeneration_, requestedGeneration_,
            true, pending_, error_, persistence_->Snapshot(), saveRejected_};
    }

    [[nodiscard]] bool Retire() noexcept
    {
        if (!OnOwnerThread()) return false;
        if (!available_) return true;
        worker_->Cancel(requestedGeneration_);
        readyTable_.reset();
        activeTable_.reset();
        worker_ = nullptr;
        presentation_ = nullptr;
        persistence_ = nullptr;
        pending_ = false;
        available_ = false;
        return true;
    }

private:
    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread_;
    }

    [[nodiscard]] static bool SameOperator(const PluginConfiguration& left,
        const PluginConfiguration& right) noexcept
    {
        if (left.algorithm != right.algorithm) return false;
        if (left.display.displayPeakNits != right.display.displayPeakNits) return false;
        return ActiveToneMapperParameters(left.algorithm, left.display.toneMapperParameters) ==
            ActiveToneMapperParameters(right.algorithm, right.display.toneMapperParameters);
    }

    [[nodiscard]] static LayerWhiteConfiguration Whites(const PluginConfiguration& value) noexcept
    {
        return {static_cast<float>(value.display.uiPaperWhiteNits),
            static_cast<float>(value.display.videoWhiteNits)};
    }

    [[nodiscard]] static SceneGradeConfiguration Grade(const PluginConfiguration& value) noexcept
    {
        return {static_cast<float>(value.display.vanillaColorGradingStrengthPercent / 100.0),
            DeriveHdrSettingsMath(value.display), value.display.colorGradingMethod};
    }

    [[nodiscard]] static OperatorLutBuildRequest BuildRequest(const PluginConfiguration& value,
        std::uint64_t generation) noexcept
    {
        return {value.algorithm, value.display.displayPeakNits, generation,
            ActiveToneMapperParameters(value.algorithm, value.display.toneMapperParameters)};
    }

    [[nodiscard]] static bool InitialResultMatches(const PluginConfiguration& value,
        const OperatorLutBuildResult& result) noexcept
    {
        if (result.request.algorithm != value.algorithm) return false;
        if (result.request.displayPeakNits != value.display.displayPeakNits) return false;
        if (ActiveToneMapperParameters(value.algorithm, value.display.toneMapperParameters) !=
            ActiveToneMapperParameters(result.request.algorithm, result.request.parameters))
            return false;
        return PreparedOperatorLutMatches(result.data, result.request);
    }

    [[nodiscard]] bool CompletionMatches(const OperatorLutBuildResult& result) const noexcept
    {
        if (result.request.requestGeneration != requestedGeneration_) return false;
        return InitialResultMatches(requested_, result);
    }

    [[nodiscard]] bool KeepRequest(const PluginConfiguration& value) const noexcept
    {
        if (!(value == requested_)) return false;
        if (pending_) return true;
        return error_ == LiveSettingsError::none;
    }

    [[nodiscard]] LiveSettingsRequestStatus PrepareRequest(const PluginConfiguration& value,
        std::uint64_t generation) noexcept
    {
        if (SameOperator(value, active_))
        {
            worker_->Cancel(requestedGeneration_);
            return LiveSettingsRequestStatus::accepted;
        }
        const auto status = worker_->Request(BuildRequest(value, generation));
        if (status != OperatorLutRequestStatus::accepted) return RejectedRequest(status);
        return LiveSettingsRequestStatus::accepted;
    }

    [[nodiscard]] const OperatorLutBuildResult* CandidateTable() noexcept
    {
        if (SameOperator(requested_, active_)) return activeTable_.get();
        if (!readyTable_) ReceiveCompleted();
        return readyTable_.get();
    }

    [[nodiscard]] bool ApplyRequested(PresentationScope scope) noexcept
    {
        if (!presentation_->CanApplySettings(scope))
        {
            error_ = LiveSettingsError::applyRejected;
            return false;
        }
        // Persisting inactive controls must not upload an identical table or stale scene data.
        if (SameNativeSettings()) return true;
        const auto* candidate = CandidateTable();
        if (!candidate) return false;
        if (presentation_->ApplySettings(
                candidate->data, Whites(requested_), scope, Grade(requested_))) return true;
        error_ = LiveSettingsError::applyRejected;
        return false;
    }

    [[nodiscard]] bool SameNativeSettings() const noexcept
    {
        if (!SameOperator(requested_, active_)) return false;
        if (requested_.display.uiPaperWhiteNits != active_.display.uiPaperWhiteNits) return false;
        if (requested_.display.videoWhiteNits != active_.display.videoWhiteNits) return false;
        const auto& requested = requested_.display;
        const auto& active = active_.display;
        if (requested.colorGradingMethod != active.colorGradingMethod) return false;
        const std::array requestedImage{requested.vanillaColorGradingStrengthPercent,
            requested.contrast, requested.blacks, requested.highlights, requested.shadows,
            requested.saturationPercent, requested.temperature, requested.tint,
            requested.exposureEv, requested.bloomStrengthPercent};
        const std::array activeImage{active.vanillaColorGradingStrengthPercent,
            active.contrast, active.blacks, active.highlights, active.shadows,
            active.saturationPercent, active.temperature, active.tint,
            active.exposureEv, active.bloomStrengthPercent};
        return requestedImage == activeImage;
    }

    [[nodiscard]] LiveSettingsRequestStatus RejectedRequest(OperatorLutRequestStatus status) noexcept
    {
        if (status == OperatorLutRequestStatus::busy)
        {
            error_ = LiveSettingsError::workerBusy;
            return LiveSettingsRequestStatus::busy;
        }
        error_ = LiveSettingsError::workerFailed;
        return LiveSettingsRequestStatus::failed;
    }

    void ReceiveCompleted() noexcept
    {
        auto result = worker_->TakeCompleted();
        if (result)
        {
            if (CompletionMatches(*result)) readyTable_ = std::move(result);
            return;
        }
        if (worker_->TakeFailedGeneration() == requestedGeneration_)
        {
            pending_ = false;
            error_ = LiveSettingsError::workerFailed;
        }
    }

    const std::thread::id ownerThread_{std::this_thread::get_id()};
    OperatorLutWorker* worker_{};
    PresentationPass* presentation_{};
    SettingsPersistence* persistence_{};
    std::unique_ptr<OperatorLutBuildResult> activeTable_;
    std::unique_ptr<OperatorLutBuildResult> readyTable_;
    PluginConfiguration active_;
    PluginConfiguration requested_;
    std::uint64_t activeGeneration_{};
    std::uint64_t requestedGeneration_{};
    std::uint64_t saveGeneration_{};
    bool available_{};
    bool pending_{};
    bool saveRejected_{};
    LiveSettingsError error_{LiveSettingsError::unavailable};
};

}  // namespace fo4hdr
