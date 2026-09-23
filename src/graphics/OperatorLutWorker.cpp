// SPDX-License-Identifier: GPL-3.0-or-later

#include "OperatorLutWorker.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace fo4hdr {
namespace {

OperatorLutBuildRequest g_request{};
std::atomic_flag g_requestLock = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> g_requestGeneration{};
std::atomic<std::uint64_t> g_failedRequestGeneration{};
std::atomic<OperatorLutBuildResult*> g_completed{};
std::atomic<bool> g_started{};
HANDLE g_requestEvent{};

[[nodiscard]] bool TryLockRequest() noexcept
{
    for (unsigned attempt = 0; attempt < 16; ++attempt)
    {
        if (!g_requestLock.test_and_set(std::memory_order_acquire)) return true;
        YieldProcessor();
    }
    return false;
}

[[nodiscard]] bool ReadStableRequest(OperatorLutBuildRequest& request) noexcept
{
    if (!TryLockRequest()) return false;
    request = g_request;
    g_requestLock.clear(std::memory_order_release);
    return true;
}

[[nodiscard]] std::unique_ptr<OperatorLutBuildResult> BuildResult(
    const OperatorLutBuildRequest& request) noexcept
{
    auto result = std::unique_ptr<OperatorLutBuildResult>(
        new (std::nothrow) OperatorLutBuildResult{});
    if (!result) return result;
    result->request = request;
    try
    {
        result->data = BuildOperatorLut(request.algorithm, request.displayPeakNits,
            request.toneMapperParameters,
            {&g_requestGeneration, request.requestGeneration});
    }
    catch (...)
    {
        result->data = {};
    }
    return result;
}

void PublishResult(std::unique_ptr<OperatorLutBuildResult> result,
    std::uint64_t requestGeneration) noexcept
{
    if (result)
    {
        auto* retired = g_completed.exchange(
            result.release(), std::memory_order_acq_rel);
        delete retired;
    }
    else
    {
        g_failedRequestGeneration.store(requestGeneration, std::memory_order_release);
    }
}

[[nodiscard]] bool IsValidRequest(const OperatorLutBuildRequest& request) noexcept
{
    HdrSettings settings{};
    settings.productionAlgorithm = request.algorithm;
    settings.displayPeakNits = request.displayPeakNits;
    settings.toneMapperParameters = request.toneMapperParameters;
    return request.requestGeneration != 0 &&
        ValidateHdrSettings(settings) == HdrSettingsValidationError::none;
}

void BuildLatestRequest(std::uint64_t& builtGeneration, DWORD& waitMilliseconds) noexcept
{
    OperatorLutBuildRequest request{};
    for (;;)
    {
        // Retry only contention; a quiescent worker needs no periodic wakeup.
        waitMilliseconds = ReadStableRequest(request) ? INFINITE : 1;
        if (waitMilliseconds != INFINITE) break;
        if (request.requestGeneration == 0 ||
            request.requestGeneration == builtGeneration)
            break;
        builtGeneration = request.requestGeneration;
        auto result = BuildResult(request);

        // A newer request makes this result irrelevant before it crosses
        // the thread boundary. Build only the newest observed value.
        if (request.requestGeneration !=
            g_requestGeneration.load(std::memory_order_acquire))
            continue;
        PublishResult(std::move(result), request.requestGeneration);
        break;
    }
}

DWORD WINAPI WorkerMain(void*) noexcept
{
    std::uint64_t builtGeneration{};
    DWORD waitMilliseconds = INFINITE;
    for (;;)
    {
        (void)WaitForSingleObject(g_requestEvent, waitMilliseconds);
        BuildLatestRequest(builtGeneration, waitMilliseconds);
    }
}

}  // namespace

bool InitializeOperatorLutWorker() noexcept
{
    if (g_started.load(std::memory_order_acquire)) return true;
    g_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_requestEvent == nullptr) return false;
    const auto thread = CreateThread(
        nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        CloseHandle(g_requestEvent);
        g_requestEvent = nullptr;
        return false;
    }
    CloseHandle(thread);
    // The plugin is process-lifetime; deliberately avoid a static jthread
    // destructor that could join while the Windows loader lock is held.
    g_started.store(true, std::memory_order_release);
    return true;
}

OperatorLutRequestStatus RequestOperatorLutBuild(
    const OperatorLutBuildRequest& request) noexcept
{
    auto canonicalRequest = request;
    canonicalRequest.toneMapperParameters = ActiveToneMapperParameters(
        request.algorithm, request.toneMapperParameters);
    if (!g_started.load(std::memory_order_acquire) ||
        !IsValidRequest(canonicalRequest)) return OperatorLutRequestStatus::failed;
    g_requestGeneration.store(
        canonicalRequest.requestGeneration, std::memory_order_release);
    if (!TryLockRequest()) return OperatorLutRequestStatus::busy;
    g_request = canonicalRequest;
    g_requestLock.clear(std::memory_order_release);
    return SetEvent(g_requestEvent) != FALSE ?
        OperatorLutRequestStatus::accepted : OperatorLutRequestStatus::failed;
}

void CancelOperatorLutBuild(std::uint64_t generation) noexcept
{
    g_requestGeneration.store(generation, std::memory_order_release);
}

std::uint64_t TakeFailedOperatorLutBuildGeneration() noexcept
{
    return g_failedRequestGeneration.exchange(0, std::memory_order_acq_rel);
}

std::unique_ptr<OperatorLutBuildResult>
TakeCompletedOperatorLutBuild() noexcept
{
    return std::unique_ptr<OperatorLutBuildResult>(
        g_completed.exchange(nullptr, std::memory_order_acq_rel));
}

}  // namespace fo4hdr
