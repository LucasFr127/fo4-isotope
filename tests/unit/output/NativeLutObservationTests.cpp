// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include "TestHarness.hpp"

#include <array>
#include <atomic>
#include <optional>
#include <thread>

namespace fo4hdr::tests {
namespace {

[[nodiscard]] NativeLutObservation MakeObservation(
    std::uint32_t threadId, std::uint32_t validSlots = 1U,
    std::uint64_t identity = 1U) noexcept
{
    NativeLutObservation result{};
    result.validSlots = validSlots;
    result.threadId = threadId;
    result.expectedShaderViews[0] = identity;
    result.luts[0].engineTextureId = identity;
    result.luts[0].resourceGeneration = identity + 100U;
    result.luts[0].weight = 1.0F;
    return result;
}

void VerifyBasicScopeAndSerials()
{
    constexpr std::uint32_t threadId = 17U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    ResetNativeLutObservation();
    FO4HDR_CHECK(CurrentNativeLutObservation().serial == 0U);

    NativeLutObservation firstCached{};
    {
        NativeLutScope first(MakeObservation(threadId, 1U, 10U));
        firstCached = CurrentNativeLutObservation();
        FO4HDR_CHECK(firstCached.serial != 0U);
        FO4HDR_CHECK(firstCached.generation != 0U);
        FO4HDR_CHECK(firstCached.expectedShaderViews[0] == 10U);
        FO4HDR_CHECK(IsCurrentNativeLutObservation(firstCached, threadId));
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(firstCached, threadId + 1U));
    }
    FO4HDR_CHECK(CurrentNativeLutObservation().serial == 0U);
    FO4HDR_CHECK(!IsCurrentNativeLutObservation(firstCached, threadId));

    {
        NativeLutScope second(MakeObservation(threadId, 1U, 20U));
        const auto secondCached = CurrentNativeLutObservation();
        FO4HDR_CHECK(secondCached.serial != firstCached.serial);
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(firstCached, threadId));
        FO4HDR_CHECK(IsCurrentNativeLutObservation(secondCached, threadId));
    }
}

void VerifyNestedScopes()
{
    constexpr std::uint32_t threadId = 23U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    ResetNativeLutObservation();
    NativeLutScope outer(MakeObservation(threadId, 1U, 30U));
    const auto outerCached = CurrentNativeLutObservation();

    {
        NativeLutScope invalid(MakeObservation(0U, 0U, 31U));
        const auto invalidCached = CurrentNativeLutObservation();
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(invalidCached, threadId));
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(outerCached, threadId));
    }
    FO4HDR_CHECK(IsCurrentNativeLutObservation(outerCached, threadId));

    {
        NativeLutScope unidentified(MakeObservation(threadId, 0U, 31U));
        const auto current = CurrentNativeLutObservation();
        FO4HDR_CHECK(IsCurrentNativeLutObservation(current, threadId));
        FO4HDR_CHECK(current.validSlots == 0U);
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(outerCached, threadId));
    }
    FO4HDR_CHECK(IsCurrentNativeLutObservation(outerCached, threadId));

    {
        NativeLutScope inner(MakeObservation(threadId, 2U, 32U));
        const auto innerCached = CurrentNativeLutObservation();
        FO4HDR_CHECK(IsCurrentNativeLutObservation(innerCached, threadId));
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(outerCached, threadId));
    }
    FO4HDR_CHECK(IsCurrentNativeLutObservation(outerCached, threadId));
}

void VerifyGenerationResetDoesNotRestoreOuter()
{
    constexpr std::uint32_t threadId = 29U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    ResetNativeLutObservation();
    NativeLutScope outer(MakeObservation(threadId, 1U, 40U));
    const auto outerCached = CurrentNativeLutObservation();
    {
        NativeLutScope inner(MakeObservation(threadId, 1U, 41U));
        FO4HDR_CHECK(IsCurrentNativeLutObservation(
            CurrentNativeLutObservation(), threadId));
        ResetNativeLutObservation();
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(
            CurrentNativeLutObservation(), threadId));
    }
    FO4HDR_CHECK(CurrentNativeLutObservation().serial == 0U);
    FO4HDR_CHECK(!IsCurrentNativeLutObservation(outerCached, threadId));
}

void VerifyThreadIsolation()
{
    constexpr std::uint32_t threadId = 37U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    ResetNativeLutObservation();
    NativeLutScope scope(MakeObservation(threadId, 1U, 50U));
    const auto cached = CurrentNativeLutObservation();
    std::atomic<bool> isolated{};
    std::thread worker([&]() {
        isolated.store(CurrentNativeLutObservation().serial == 0U &&
                !IsCurrentNativeLutObservation(cached, threadId),
            std::memory_order_release);
    });
    worker.join();
    FO4HDR_CHECK(isolated.load(std::memory_order_acquire));
    FO4HDR_CHECK(IsCurrentNativeLutObservation(cached, threadId));
}

void VerifyImageSpaceRevocation()
{
    constexpr std::uint32_t threadId = 39U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    SetImageSpaceObservationReady(true);
    auto observation = MakeObservation(threadId, 1U, 55U);
    observation.imageSpaceValidSlots = 1U;
    observation.imageSpaceStatus = ImageSpaceObservationStatus::matched;
    observation.imageSpaceRevision = CurrentImageSpaceObservationRevision();
    NativeLutScope scope(observation);
    const auto copied = CurrentNativeLutObservation();
    FO4HDR_CHECK(EligibleImageSpaceSlots(copied, 1U) == 1U);
    FO4HDR_CHECK(EligibleImageSpaceSlots(copied, 0U) == 0U);
    std::thread writer([] { SetImageSpaceObservationReady(false); });
    writer.join();
    FO4HDR_CHECK(EligibleImageSpaceSlots(copied, 1U) == 0U);
    FO4HDR_CHECK(IsCurrentNativeLutObservation(copied, threadId));
}

void VerifyImageSpaceRevisionInvalidation()
{
    constexpr std::uint32_t threadId = 41U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    SetImageSpaceObservationReady(true);
    auto observation = MakeObservation(threadId, 1U, 57U);
    observation.imageSpaceValidSlots = 1U;
    observation.imageSpaceStatus = ImageSpaceObservationStatus::matched;
    observation.imageSpaceRevision = CurrentImageSpaceObservationRevision();
    NativeLutScope scope(observation);
    const auto copied = CurrentNativeLutObservation();
    FO4HDR_CHECK(EligibleImageSpaceSlots(copied, 1U) == 1U);

    const auto revision = InvalidateImageSpaceObservation();
    FO4HDR_CHECK(IsImageSpaceObservationReady());
    FO4HDR_CHECK(revision == CurrentImageSpaceObservationRevision());
    FO4HDR_CHECK(EligibleImageSpaceSlots(copied, 1U) == 0U);

    auto current = copied;
    current.imageSpaceRevision = revision;
    FO4HDR_CHECK(EligibleImageSpaceSlots(current, 1U) == 1U);
}

void VerifyUnavailableStates()
{
    constexpr std::uint32_t threadId = 43U;
    constexpr std::array unavailableStates{
        NativeLutObserverStatus::unavailable,
        NativeLutObserverStatus::unsupportedRuntime,
        NativeLutObserverStatus::invalidTarget,
        NativeLutObserverStatus::conflictingHook};
    for (const auto status : unavailableStates)
    {
        SetNativeLutObserverStatus(status);
        FO4HDR_CHECK(GetNativeLutObserverStatus() == status);
        NativeLutScope scope(MakeObservation(threadId, 1U, 60U));
        FO4HDR_CHECK(CurrentNativeLutObservation().serial == 0U);
        FO4HDR_CHECK(!IsCurrentNativeLutObservation(
            CurrentNativeLutObservation(), threadId));
    }
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
}

void VerifyBoundedNesting()
{
    constexpr std::uint32_t threadId = 47U;
    SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    ResetNativeLutObservation();
    std::array<std::optional<NativeLutScope>, 9> scopes;
    std::array<NativeLutObservation, 8> observations{};
    for (std::size_t depth = 0; depth < observations.size(); ++depth)
    {
        scopes[depth].emplace(MakeObservation(
            threadId, 1U, 70U + depth));
        observations[depth] = CurrentNativeLutObservation();
        FO4HDR_CHECK(IsCurrentNativeLutObservation(
            observations[depth], threadId));
    }

    scopes[8].emplace(MakeObservation(threadId, 1U, 90U));
    FO4HDR_CHECK(CurrentNativeLutObservation().serial == 0U);
    FO4HDR_CHECK(!IsCurrentNativeLutObservation(observations[7], threadId));
    scopes[8].reset();
    FO4HDR_CHECK(IsCurrentNativeLutObservation(observations[7], threadId));

    for (std::size_t depth = observations.size(); depth-- > 0;)
    {
        scopes[depth].reset();
        if (depth > 0)
            FO4HDR_CHECK(IsCurrentNativeLutObservation(
                observations[depth - 1], threadId));
    }
    FO4HDR_CHECK(CurrentNativeLutObservation().serial == 0U);
}

}  // namespace

void RunNativeLutObservationTests()
{
    VerifyBasicScopeAndSerials();
    VerifyNestedScopes();
    VerifyGenerationResetDoesNotRestoreOuter();
    VerifyThreadIsolation();
    VerifyImageSpaceRevocation();
    VerifyImageSpaceRevisionInvalidation();
    VerifyUnavailableStates();
    VerifyBoundedNesting();
    SetNativeLutObserverStatus(NativeLutObserverStatus::unavailable);
}

}  // namespace fo4hdr::tests
