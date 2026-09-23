// SPDX-License-Identifier: GPL-3.0-or-later
#include "OperatorLutWorker.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <chrono>
#include <thread>

namespace fo4hdr::tests {
namespace {
std::unique_ptr<OperatorLutBuildResult> WaitForTable()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto result = TakeCompletedOperatorLutBuild()) return result;
        if (TakeFailedOperatorLutBuildGeneration()) return {};
        std::this_thread::yield();
    }
    return {};
}
} // namespace

void RunHdrOperatorWorkerTests()
{
    const OperatorLutBuildRequest request{ProductionToneMapperAlgorithm::gt7, 1000, 17, 23, 1};
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(request, request, true) == OperatorLutCompletionAction::publish);
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(request, request, false) == OperatorLutCompletionAction::retire);
    auto stale = request;
    stale.requestGeneration = 2;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(stale, request, true) == OperatorLutCompletionAction::retire);
    const auto initialized = InitializeOperatorLutWorker();
    FO4HDR_CHECK(initialized);
    if (!initialized) return;
    const auto submitted = RequestOperatorLutBuild(request);
    FO4HDR_CHECK(submitted == OperatorLutRequestStatus::accepted);
    if (submitted != OperatorLutRequestStatus::accepted) return;
    const auto result = WaitForTable();
    FO4HDR_CHECK(result);
    if (!result) return;
    FO4HDR_CHECK(result->request.deviceGeneration == 17 && result->request.settingsGeneration == 23);
    FO4HDR_CHECK(result->request.requestGeneration == 1);
    FO4HDR_CHECK(PreparedOperatorLutMatches(result->data, request));
    FO4HDR_CHECK(!TakeCompletedOperatorLutBuild());
    CancelOperatorLutBuild(2);
}
} // namespace fo4hdr::tests
