// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/config/HdrSettings.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"

#include <cstdint>
#include <memory>

namespace fo4hdr {

struct OperatorLutBuildRequest
{
    ProductionToneMapperAlgorithm algorithm{};
    double displayPeakNits{};
    std::uint64_t deviceGeneration{};
    std::uint64_t settingsGeneration{};
    std::uint64_t requestGeneration{};
    ToneMapperParameters toneMapperParameters{};
};

struct OperatorLutBuildResult
{
    OperatorLutBuildRequest request{};
    OperatorLutData data{};
};

[[nodiscard]] inline bool PreparedOperatorLutMatches(
    const OperatorLutData& data,
    const OperatorLutBuildRequest& request) noexcept
{
    return data.IsValid() && data.algorithm == request.algorithm &&
        data.displayPeakNits == request.displayPeakNits &&
        data.toneMapperParameters == ActiveToneMapperParameters(
            request.algorithm, request.toneMapperParameters);
}

enum class OperatorLutCompletionAction : std::uint8_t
{
    retire,
    publish
};

[[nodiscard]] constexpr bool OperatorLutContentMatches(
    const OperatorLutBuildRequest& left,
    const OperatorLutBuildRequest& right) noexcept
{
    return left.algorithm == right.algorithm &&
        left.displayPeakNits == right.displayPeakNits &&
        ActiveToneMapperParameters(left.algorithm, left.toneMapperParameters) ==
            ActiveToneMapperParameters(right.algorithm, right.toneMapperParameters);
}

// Classify a completed CPU build without touching worker or D3D state.
// CPU data has no device identity; upload it only to the current device.
// Stale completions are retired because a newer content-key request owns
// publication. The request-time settings generation is diagnostic only:
// Image/Effects constants may advance while the same LUT is being built.
[[nodiscard]] constexpr OperatorLutCompletionAction
ClassifyOperatorLutCompletion(
    const OperatorLutBuildRequest& completed,
    const OperatorLutBuildRequest& desired,
    bool pending) noexcept
{
    if (!pending ||
        completed.requestGeneration != desired.requestGeneration)
        return OperatorLutCompletionAction::retire;
    if (!OperatorLutContentMatches(completed, desired))
        return OperatorLutCompletionAction::retire;
    return OperatorLutCompletionAction::publish;
}

enum class OperatorLutRequestStatus : std::uint8_t
{
    accepted,
    busy,
    failed
};

// Contention is retried on later Present boundaries, never by waiting.
[[nodiscard]] constexpr bool RetryOperatorLutSubmission(
    OperatorLutRequestStatus status, unsigned& attempts) noexcept
{
    return status == OperatorLutRequestStatus::busy && ++attempts < 4;
}

// One process-lifetime CPU worker with a replace-latest request slot. D3D
// creation and publication remain exclusively on the render thread.
[[nodiscard]] bool InitializeOperatorLutWorker() noexcept;
[[nodiscard]] OperatorLutRequestStatus RequestOperatorLutBuild(
    const OperatorLutBuildRequest& request) noexcept;
void CancelOperatorLutBuild(std::uint64_t generation) noexcept;
[[nodiscard]] std::unique_ptr<OperatorLutBuildResult>
TakeCompletedOperatorLutBuild() noexcept;
[[nodiscard]] std::uint64_t TakeFailedOperatorLutBuildGeneration() noexcept;

}  // namespace fo4hdr
