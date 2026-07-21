// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/tonemap/OperatorLut.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace fo4hdr {

struct OperatorLutBuildRequest
{
    OperatorLutAlgorithm algorithm{};
    double displayPeakNits{};
    std::uint64_t requestGeneration{};
};

struct OperatorLutBuildResult
{
    OperatorLutBuildRequest request{};
    OperatorLutData data{};
};

[[nodiscard]] bool PreparedOperatorLutMatches(const OperatorLutData& data,
    const OperatorLutBuildRequest& request) noexcept;

enum class OperatorLutRequestStatus : std::uint8_t
{
    accepted,
    busy,
    failed
};

// One caller owns initialization, submission, cancellation and consumption.
// Use increasing nonzero request generations. Destruction cancels and joins
// on the cold owner path; the worker must not be destroyed under loader lock.
class OperatorLutWorker
{
public:
    ~OperatorLutWorker() noexcept;
    [[nodiscard]] bool Initialize() noexcept;
    [[nodiscard]] OperatorLutRequestStatus Request(
        const OperatorLutBuildRequest& request) noexcept;
    void Cancel(std::uint64_t generation) noexcept;
    [[nodiscard]] std::unique_ptr<OperatorLutBuildResult> TakeCompleted() noexcept;
    [[nodiscard]] std::uint64_t TakeFailedGeneration() noexcept;

private:
    void Run() noexcept;
    void Publish(std::unique_ptr<OperatorLutBuildResult> result,
        const OperatorLutBuildRequest& request) noexcept;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<std::uint64_t> generation_{};
    std::optional<OperatorLutBuildRequest> pending_;
    std::unique_ptr<OperatorLutBuildResult> completed_;
    std::uint64_t failedGeneration_{};
    std::uint64_t lastGeneration_{};
    bool stopping_{};
};

}  // namespace fo4hdr
