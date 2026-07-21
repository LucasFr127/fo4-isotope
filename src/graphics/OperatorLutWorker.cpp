// SPDX-License-Identifier: GPL-3.0-or-later

#include "OperatorLutWorker.hpp"

#include "Fallout4HDR/output/Hdr10Color.hpp"

#include <cmath>
#include <new>
#include <utility>

namespace fo4hdr {
namespace {

bool ValidAlgorithm(OperatorLutAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
    case OperatorLutAlgorithm::aces2:
    case OperatorLutAlgorithm::agX:
    case OperatorLutAlgorithm::gt7: return true;
    default: return false;
    }
}

bool ValidRequest(const OperatorLutBuildRequest& request) noexcept
{
    return request.requestGeneration != 0 && ValidAlgorithm(request.algorithm) &&
        std::isfinite(request.displayPeakNits) &&
        request.displayPeakNits >= kMinimumDisplayPeakNits &&
        request.displayPeakNits <= kMaximumDisplayPeakNits;
}

std::unique_ptr<OperatorLutBuildResult> BuildResult(
    const OperatorLutBuildRequest& request,
    const std::atomic<std::uint64_t>& generation) noexcept
{
    auto result = std::unique_ptr<OperatorLutBuildResult>(
        new (std::nothrow) OperatorLutBuildResult{});
    if (!result) return result;
    result->request = request;
    result->data = BuildOperatorLut(request.algorithm, request.displayPeakNits,
        {&generation, request.requestGeneration});
    return result;
}

}  // namespace

bool PreparedOperatorLutMatches(const OperatorLutData& data,
    const OperatorLutBuildRequest& request) noexcept
{
    return ValidRequest(request) && data.IsValid() &&
        data.algorithm == request.algorithm && data.displayPeakNits == request.displayPeakNits;
}

OperatorLutWorker::~OperatorLutWorker() noexcept
{
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
        generation_.store(0, std::memory_order_release);
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

bool OperatorLutWorker::Initialize() noexcept
{
    if (thread_.joinable()) return true;
    try
    {
        thread_ = std::thread{&OperatorLutWorker::Run, this};
        return true;
    }
    catch (...) { return false; }
}

OperatorLutRequestStatus OperatorLutWorker::Request(
    const OperatorLutBuildRequest& request) noexcept
{
    if (!thread_.joinable() || !ValidRequest(request)) return OperatorLutRequestStatus::failed;
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return OperatorLutRequestStatus::busy;
    if (request.requestGeneration <= lastGeneration_) return OperatorLutRequestStatus::failed;
    pending_ = request;
    lastGeneration_ = request.requestGeneration;
    generation_.store(request.requestGeneration, std::memory_order_release);
    wake_.notify_one();
    return OperatorLutRequestStatus::accepted;
}

void OperatorLutWorker::Cancel(std::uint64_t generation) noexcept
{
    generation_.compare_exchange_strong(generation, 0, std::memory_order_acq_rel);
}

std::unique_ptr<OperatorLutBuildResult> OperatorLutWorker::TakeCompleted() noexcept
{
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return {};
    if (!completed_) return {};
    auto result = std::move(completed_);
    if (result->request.requestGeneration != generation_.load(std::memory_order_acquire)) return {};
    return result;
}

std::uint64_t OperatorLutWorker::TakeFailedGeneration() noexcept
{
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) return 0;
    const auto failed = std::exchange(failedGeneration_, 0);
    return failed == generation_.load(std::memory_order_acquire) ? failed : 0;
}

void OperatorLutWorker::Publish(std::unique_ptr<OperatorLutBuildResult> result,
    const OperatorLutBuildRequest& request) noexcept
{
    const std::lock_guard lock{mutex_};
    if (request.requestGeneration != generation_.load(std::memory_order_acquire)) return;
    if (!result || !PreparedOperatorLutMatches(result->data, request))
    {
        failedGeneration_ = request.requestGeneration;
        return;
    }
    completed_ = std::move(result);
}

void OperatorLutWorker::Run() noexcept
{
    for (;;)
    {
        std::unique_lock lock{mutex_};
        wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
        if (stopping_) return;
        const auto request = *pending_;
        pending_.reset();
        lock.unlock();
        Publish(BuildResult(request, generation_), request);
    }
}

}  // namespace fo4hdr
