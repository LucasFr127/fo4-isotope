// SPDX-License-Identifier: GPL-3.0-or-later

#include "OperatorLutWorker.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <thread>

namespace fo4hdr::tests {
namespace {

constexpr OperatorLutBuildRequest first{OperatorLutAlgorithm::gt7, 1000.0, 1};
constexpr OperatorLutBuildRequest second{OperatorLutAlgorithm::gt7, 1000.0, 2};
constexpr OperatorLutBuildRequest third{OperatorLutAlgorithm::gt7, 1000.0, 3};
constexpr OperatorLutBuildRequest fourth{OperatorLutAlgorithm::gt7, 1000.0, 4};

void CheckPackedOperatorTable(const OperatorLutData& data)
{
    FO4HDR_CHECK(data.edge == 64);
    FO4HDR_CHECK(data.textureEdge == data.edge + 2);
    std::size_t nonzero{};
    for (std::size_t index = 0; index < data.rgba16.size(); ++index)
    {
        const auto packed = data.rgba16[index];
        FO4HDR_CHECK((packed & 0x7C00U) != 0x7C00U);
        if (index % 4 == 3) FO4HDR_CHECK(packed == 0x3C00U);
        else if ((packed & 0x7FFFU) != 0) ++nonzero;
    }
    FO4HDR_CHECK(nonzero > 0);
}

std::unique_ptr<OperatorLutBuildResult> WaitForOperatorTable(OperatorLutWorker& worker)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto result = worker.TakeCompleted()) return result;
        if (worker.TakeFailedGeneration() != 0) return {};
        std::this_thread::yield();
    }
    return {};
}

OperatorLutRequestStatus SubmitOperatorTable(OperatorLutWorker& worker,
    const OperatorLutBuildRequest& request)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    do
    {
        const auto status = worker.Request(request);
        if (status != OperatorLutRequestStatus::busy) return status;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return OperatorLutRequestStatus::busy;
}

bool SubmitAccepted(OperatorLutWorker& worker, const OperatorLutBuildRequest& request)
{
    const auto status = SubmitOperatorTable(worker, request);
    FO4HDR_CHECK(status == OperatorLutRequestStatus::accepted);
    return status == OperatorLutRequestStatus::accepted;
}

std::unique_ptr<OperatorLutBuildResult> TakeMatchingResult(OperatorLutWorker& worker,
    const OperatorLutBuildRequest& request)
{
    auto result = WaitForOperatorTable(worker);
    FO4HDR_CHECK(result);
    if (!result) return {};
    FO4HDR_CHECK(result->request.requestGeneration == request.requestGeneration);
    FO4HDR_CHECK(PreparedOperatorLutMatches(result->data, request));
    FO4HDR_CHECK(!worker.TakeCompleted());
    return result;
}

void CheckInvalidRequests(OperatorLutWorker& worker)
{
    constexpr auto invalid = static_cast<OperatorLutAlgorithm>(0xFF);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    for (const auto request : std::array{
             OperatorLutBuildRequest{invalid, 1000.0, 1},
             OperatorLutBuildRequest{OperatorLutAlgorithm::gt7, 1000.0, 0}})
        FO4HDR_CHECK(worker.Request(request) == OperatorLutRequestStatus::failed);
    for (double peak : {399.0, 2001.0, nan, infinity})
        FO4HDR_CHECK(worker.Request({OperatorLutAlgorithm::gt7, peak, 1}) ==
            OperatorLutRequestStatus::failed);
}

bool CheckReplaceLatest(OperatorLutWorker& worker)
{
    if (!SubmitAccepted(worker, first)) return false;
    if (!SubmitAccepted(worker, second)) return false;
    // Both requests precede consumption, independent of which build finished first.
    worker.Cancel(first.requestGeneration);
    const auto result = TakeMatchingResult(worker, second);
    if (!result) return false;
    FO4HDR_CHECK(SubmitOperatorTable(worker, second) == OperatorLutRequestStatus::failed);
    FO4HDR_CHECK(SubmitOperatorTable(worker, first) == OperatorLutRequestStatus::failed);
    return true;
}

void CheckCancellation(OperatorLutWorker& worker)
{
    if (!SubmitAccepted(worker, third)) return;
    worker.Cancel(third.requestGeneration);
    FO4HDR_CHECK(!worker.TakeCompleted());
    if (!SubmitAccepted(worker, fourth)) return;
    const auto result = TakeMatchingResult(worker, fourth);
    if (!result) return;
    CheckPackedOperatorTable(result->data);
    auto mismatch = fourth;
    mismatch.algorithm = OperatorLutAlgorithm::agX;
    FO4HDR_CHECK(!PreparedOperatorLutMatches(result->data, mismatch));
    mismatch = fourth;
    mismatch.displayPeakNits = 400.0;
    FO4HDR_CHECK(!PreparedOperatorLutMatches(result->data, mismatch));
}

}  // namespace

void RunHdrOperatorWorkerTests()
{
    OperatorLutWorker worker;
    FO4HDR_CHECK(worker.Request(first) == OperatorLutRequestStatus::failed);
    const auto initialized = worker.Initialize();
    FO4HDR_CHECK(initialized);
    if (!initialized) return;
    CheckInvalidRequests(worker);
    if (!CheckReplaceLatest(worker)) return;
    CheckCancellation(worker);
}

}  // namespace fo4hdr::tests
