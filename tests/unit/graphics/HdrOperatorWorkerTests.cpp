// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"

#include "OperatorLutWorker.hpp"
#include "TestHarness.hpp"
#include <Windows.h>
#include <memory>

namespace fo4hdr::tests {

void TestOperatorParameterMatching(const OperatorLutBuildRequest& desired)
{
    for (const auto algorithm : {
             ProductionToneMapperAlgorithm::aces2Lmt,
             ProductionToneMapperAlgorithm::agX,
             ProductionToneMapperAlgorithm::gt7})
    {
        auto parameterDesired = desired;
        parameterDesired.algorithm = algorithm;
        parameterDesired.toneMapperParameters = {};
        auto selectedMismatch = parameterDesired;
        auto inactiveMismatch = parameterDesired;
        switch (algorithm)
        {
        case ProductionToneMapperAlgorithm::aces2Lmt:
            selectedMismatch.toneMapperParameters.acesLookStrength = 0.5;
            inactiveMismatch.toneMapperParameters.agxContrast = 3.0;
            break;
        case ProductionToneMapperAlgorithm::agX:
            selectedMismatch.toneMapperParameters.agxContrast = 3.0;
            inactiveMismatch.toneMapperParameters.acesLookStrength = 0.5;
            break;
        case ProductionToneMapperAlgorithm::gt7:
            selectedMismatch.toneMapperParameters.gt7ToeStrength = 1.5;
            inactiveMismatch.toneMapperParameters.acesLookStrength = 0.5;
            break;
        }
        FO4HDR_CHECK(ClassifyOperatorLutCompletion(
            selectedMismatch, parameterDesired, true) ==
            OperatorLutCompletionAction::retire);
        FO4HDR_CHECK(ClassifyOperatorLutCompletion(
            inactiveMismatch, parameterDesired, true) ==
            OperatorLutCompletionAction::publish);
    }
}

void TestOperatorLutDeviceReplacementTransitions()
{
    const OperatorLutBuildRequest desired{
        .algorithm = ProductionToneMapperAlgorithm::gt7,
        .displayPeakNits = 430.0,
        .deviceGeneration = 12,
        .settingsGeneration = 8,
        .requestGeneration = 5
    };

    auto completed = desired;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, desired, true) ==
        OperatorLutCompletionAction::publish);

    TestOperatorParameterMatching(desired);

    // CPU coefficients survive device replacement; upload to the new device
    // without repeating the bake or changing the user's request generation.
    completed.deviceGeneration = 11;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, desired, true) ==
        OperatorLutCompletionAction::publish);

    // A newer content request still retires an earlier completion.
    auto requeued = desired;
    requeued.requestGeneration = 6;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, requeued, true) ==
        OperatorLutCompletionAction::retire);
    completed = requeued;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, requeued, true) ==
        OperatorLutCompletionAction::publish);

    // Cancellation/reversion owns retirement even if an exact completion was
    // already waiting in the worker slot.
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, requeued, false) ==
        OperatorLutCompletionAction::retire);

    // Constant-only settings revisions do not change LUT content or invalidate
    // an otherwise current completion.
    auto newerConstants = requeued;
    ++newerConstants.settingsGeneration;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, newerConstants, true) ==
        OperatorLutCompletionAction::publish);

    auto lmtDesired = requeued;
    lmtDesired.algorithm = ProductionToneMapperAlgorithm::aces2Lmt;
    ++lmtDesired.settingsGeneration;
    ++lmtDesired.requestGeneration;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, lmtDesired, true) ==
        OperatorLutCompletionAction::retire);
    completed = lmtDesired;
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(
        completed, lmtDesired, true) ==
        OperatorLutCompletionAction::publish);

    unsigned attempts{};
    for (unsigned retry = 0; retry < 3; ++retry)
        FO4HDR_CHECK(RetryOperatorLutSubmission(
            OperatorLutRequestStatus::busy, attempts));
    FO4HDR_CHECK(!RetryOperatorLutSubmission(
        OperatorLutRequestStatus::busy, attempts));
    attempts = 0;
    FO4HDR_CHECK(RetryOperatorLutSubmission(
        OperatorLutRequestStatus::busy, attempts));
    FO4HDR_CHECK(!RetryOperatorLutSubmission(
        OperatorLutRequestStatus::accepted, attempts));
    FO4HDR_CHECK(attempts == 1);
    FO4HDR_CHECK(!RetryOperatorLutSubmission(
        OperatorLutRequestStatus::failed, attempts));
}

std::unique_ptr<OperatorLutBuildResult> WaitForPreparedOperatorLut()
{
    std::unique_ptr<OperatorLutBuildResult> prepared;
    for (unsigned wait = 0; wait < 6000 && !prepared; ++wait)
    {
        prepared = TakeCompletedOperatorLutBuild();
        if (prepared && prepared->request.requestGeneration != 3)
            prepared.reset();
        if (!prepared) Sleep(10);
    }
    return prepared;
}

void TestOperatorLutWorkerAndPreparedCache()
{
    FO4HDR_CHECK(InitializeOperatorLutWorker());
    OperatorLutBuildRequest request{
        .algorithm = ProductionToneMapperAlgorithm::gt7,
        .displayPeakNits = 430.0,
        .deviceGeneration = 1,
        .settingsGeneration = 1,
        .requestGeneration = 1
    };
    auto invalid = request;
    invalid.requestGeneration = 0;
    FO4HDR_CHECK(RequestOperatorLutBuild(invalid) ==
        OperatorLutRequestStatus::failed);
    FO4HDR_CHECK(!PreparedOperatorLutMatches({}, request));
    FO4HDR_CHECK(RequestOperatorLutBuild(request) ==
        OperatorLutRequestStatus::accepted);
    CancelOperatorLutBuild(2);
    request.requestGeneration = 3;
    request.displayPeakNits = 440.0;
    auto status = RequestOperatorLutBuild(request);
    for (unsigned retry = 0;
         retry < 4 && status == OperatorLutRequestStatus::busy; ++retry)
    {
        Sleep(1);
        status = RequestOperatorLutBuild(request);
    }
    FO4HDR_CHECK(status == OperatorLutRequestStatus::accepted);
    auto prepared = WaitForPreparedOperatorLut();
    FO4HDR_CHECK(prepared != nullptr);
    if (!prepared) return;
    FO4HDR_CHECK(TakeFailedOperatorLutBuildGeneration() == 0);
    FO4HDR_CHECK(PreparedOperatorLutMatches(prepared->data, request));
    const auto* coefficients = prepared->data.rgba16.data();
    ++request.deviceGeneration;
    ++request.settingsGeneration;
    FO4HDR_CHECK(PreparedOperatorLutMatches(prepared->data, request));
    FO4HDR_CHECK(ClassifyOperatorLutCompletion(prepared->request, request, true) ==
        OperatorLutCompletionAction::publish);
    FO4HDR_CHECK(prepared->data.rgba16.data() == coefficients);
    request.toneMapperParameters.acesLookStrength = 0.5;
    FO4HDR_CHECK(PreparedOperatorLutMatches(prepared->data, request));
    request.toneMapperParameters.gt7ToeStrength = 1.5;
    FO4HDR_CHECK(!PreparedOperatorLutMatches(prepared->data, request));
    request.toneMapperParameters = {};
    request.displayPeakNits = 450.0;
    FO4HDR_CHECK(!PreparedOperatorLutMatches(prepared->data, request));
}

}  // namespace fo4hdr::tests
