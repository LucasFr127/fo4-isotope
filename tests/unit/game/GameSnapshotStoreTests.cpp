// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameSnapshotStore.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <Windows.h>
#include <array>
#include <cstring>
#include <limits>
#include <barrier>
#include <cstdint>
#include <thread>

namespace {

void TestInitialSnapshot()
{
    const fo4hdr::GameSnapshotStore store;
    const auto snapshot = store.Read();
    FO4HDR_CHECK(snapshot.generation == 0);
    FO4HDR_CHECK(snapshot.resourceGeneration == 0);
    FO4HDR_CHECK(snapshot.lifecycleGeneration == 0);
    FO4HDR_CHECK(snapshot.observationGeneration == 0);
    FO4HDR_CHECK(snapshot.lifecycleEvent == fo4hdr::GameLifecycleEvent::None);
    FO4HDR_CHECK(snapshot.collectorThreadId == 0);
    FO4HDR_CHECK(!snapshot.messagingAvailable);
    FO4HDR_CHECK(!snapshot.lifecycleSucceeded);
    FO4HDR_CHECK(!snapshot.loading);
    FO4HDR_CHECK(!snapshot.taskCollectionAvailable);
    FO4HDR_CHECK(!snapshot.collectorFailed);
    FO4HDR_CHECK(!snapshot.rendererInitialized);
    FO4HDR_CHECK(!snapshot.rendererBorderless);
    FO4HDR_CHECK(!snapshot.rendererVsync);
    FO4HDR_CHECK(snapshot.rendererDeviceIdentity == 0);
    FO4HDR_CHECK(snapshot.rendererContextIdentity == 0);
}

void TestOwnedReadCopy()
{
    fo4hdr::GameSnapshotStore store;
    store.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        snapshot.rendererInitialized = true;
        snapshot.rendererDeviceIdentity = 17;
        snapshot.rendererContextIdentity = 23;
    });
    const auto before = store.Read();
    auto copy = store.Read();
    copy.generation = 99;
    copy.rendererInitialized = false;
    copy.rendererDeviceIdentity = 42;
    FO4HDR_CHECK(copy.rendererDeviceIdentity == 42);
    const auto unchanged = store.Read();
    FO4HDR_CHECK(unchanged.generation == 1);
    FO4HDR_CHECK(unchanged.rendererInitialized);
    FO4HDR_CHECK(unchanged.rendererDeviceIdentity == 17);
    FO4HDR_CHECK(unchanged.rendererContextIdentity == 23);

    store.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept { snapshot = {}; });
    FO4HDR_CHECK(before.generation == 1);
    FO4HDR_CHECK(before.rendererInitialized);
    FO4HDR_CHECK(before.rendererDeviceIdentity == 17);
    FO4HDR_CHECK(before.rendererContextIdentity == 23);
    FO4HDR_CHECK(store.Read().rendererDeviceIdentity == 0);
}

void TestReplacementGeneration()
{
    fo4hdr::GameSnapshotStore store;
    for (std::uint64_t generation = 1; generation <= 3; ++generation)
    {
        store.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            snapshot = {};
            snapshot.generation = 99;
        });
        FO4HDR_CHECK(store.Read().generation == generation);
    }
}

void CheckCoherentSnapshot(const fo4hdr::GameHdrSnapshot& snapshot)
{
    const auto sequence = snapshot.rendererDeviceIdentity;
    FO4HDR_CHECK(snapshot.generation == sequence);
    FO4HDR_CHECK(snapshot.rendererInitialized == (sequence != 0));
    FO4HDR_CHECK(snapshot.rendererBorderless == (sequence % 2 != 0));
    FO4HDR_CHECK(snapshot.rendererVsync == (sequence != 0 && sequence % 3 == 0));
    if (sequence != 0)
    {
        FO4HDR_CHECK(snapshot.rendererContextIdentity == ~sequence);
    }
    else
    {
        FO4HDR_CHECK(snapshot.rendererContextIdentity == 0);
    }
}

void TestCoherentConcurrentReads()
{
    constexpr std::uint64_t iterations = 1000;
    fo4hdr::GameSnapshotStore store;
    std::barrier start{2};
    std::jthread writer([&] {
        start.arrive_and_wait();
        for (std::uint64_t sequence = 1; sequence <= iterations; ++sequence)
        {
            store.Update([sequence](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                snapshot.rendererInitialized = true;
                snapshot.rendererBorderless = sequence % 2 != 0;
                snapshot.rendererVsync = sequence % 3 == 0;
                snapshot.rendererDeviceIdentity = sequence;
                snapshot.rendererContextIdentity = ~sequence;
            });
        }
    });
    start.arrive_and_wait();
    std::uint64_t previous = 0;
    for (std::uint64_t read = 0; read < iterations; ++read)
    {
        const auto snapshot = store.Read();
        CheckCoherentSnapshot(snapshot);
        FO4HDR_CHECK(snapshot.generation >= previous);
        previous = snapshot.generation;
    }
    writer.join();
    const auto final = store.Read();
    CheckCoherentSnapshot(final);
    FO4HDR_CHECK(final.generation == iterations);
}

void TestConcurrentWriters()
{
    constexpr std::uint64_t iterations = 1000;
    fo4hdr::GameSnapshotStore store;
    std::barrier start{2};
    const auto increment = [&] {
        start.arrive_and_wait();
        for (std::uint64_t mutation = 0; mutation < iterations; ++mutation)
        {
            store.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                ++snapshot.rendererDeviceIdentity;
            });
        }
    };
    std::jthread first(increment);
    increment();
    first.join();
    const auto snapshot = store.Read();
    FO4HDR_CHECK(snapshot.rendererDeviceIdentity == 2 * iterations);
    FO4HDR_CHECK(snapshot.generation == 2 * iterations);
}

void TestLifecycleOutcome()
{
    using Event = fo4hdr::GameLifecycleEvent;
    for (const auto event : {Event::PostLoadGame, Event::GameDataReady})
    {
        FO4HDR_CHECK(fo4hdr::DecodeGameLifecycleOutcome(event, 1, 0) == false);
        FO4HDR_CHECK(fo4hdr::DecodeGameLifecycleOutcome(event, 1, 1) == true);
        FO4HDR_CHECK(!fo4hdr::DecodeGameLifecycleOutcome(event, 0, 0));
        FO4HDR_CHECK(!fo4hdr::DecodeGameLifecycleOutcome(event, 2, 1));
        FO4HDR_CHECK(!fo4hdr::DecodeGameLifecycleOutcome(event, 1, 2));
    }
    FO4HDR_CHECK(fo4hdr::DecodeGameLifecycleOutcome(Event::PreSaveGame, 12, 42) == true);
    FO4HDR_CHECK(fo4hdr::ShouldCollectGameSnapshotAtLifecycle(Event::PostLoadGame, true));
    FO4HDR_CHECK(!fo4hdr::ShouldCollectGameSnapshotAtLifecycle(Event::PostLoadGame, false));
    FO4HDR_CHECK(fo4hdr::ShouldCollectGameSnapshotAtLifecycle(Event::NewGame, true));
    FO4HDR_CHECK(fo4hdr::ShouldCollectGameSnapshotAtLifecycle(Event::PreSaveGame, true));
    FO4HDR_CHECK(!fo4hdr::ShouldCollectGameSnapshotAtLifecycle(Event::PreLoadGame, true));
    FO4HDR_CHECK(!fo4hdr::ShouldCollectGameSnapshotAtLifecycle(Event::GameDataReady, true));
}

void TestLifecycleTransitions()
{
    using Event = fo4hdr::GameLifecycleEvent;
    fo4hdr::GameHdrSnapshot snapshot{};
    snapshot.rendererInitialized = true;
    snapshot.rendererBorderless = true;
    snapshot.rendererVsync = true;
    snapshot.rendererDeviceIdentity = 17;
    snapshot.rendererContextIdentity = 23;
    fo4hdr::ApplyGameLifecycleTransition(snapshot, Event::PreLoadGame, true, 0);
    FO4HDR_CHECK(snapshot.loading);
    FO4HDR_CHECK(snapshot.resourceGeneration == 1);
    FO4HDR_CHECK(!snapshot.rendererInitialized);
    FO4HDR_CHECK(!snapshot.rendererBorderless);
    FO4HDR_CHECK(!snapshot.rendererVsync);
    FO4HDR_CHECK(snapshot.rendererDeviceIdentity == 0);
    FO4HDR_CHECK(snapshot.rendererContextIdentity == 0);
    fo4hdr::ApplyGameLifecycleTransition(snapshot, Event::PostLoadGame, false, 0);
    FO4HDR_CHECK(snapshot.loading);
    FO4HDR_CHECK(!snapshot.lifecycleSucceeded);
    fo4hdr::ApplyGameLifecycleTransition(snapshot, Event::PostLoadGame, true, 0);
    FO4HDR_CHECK(!snapshot.loading);
    FO4HDR_CHECK(snapshot.lifecycleSucceeded);
    FO4HDR_CHECK(snapshot.resourceGeneration == 1);
    fo4hdr::ApplyGameLifecycleTransition(snapshot, Event::NewGame, true, 0);
    FO4HDR_CHECK(snapshot.loading);
    FO4HDR_CHECK(snapshot.resourceGeneration == 2);
    FO4HDR_CHECK(snapshot.lifecycleGeneration == 4);
    FO4HDR_CHECK(snapshot.lifecycleEvent == Event::NewGame);
}

void TestGraphicsLifecyclePublication()
{
    using Event = fo4hdr::GameLifecycleEvent;
    fo4hdr::GraphicsLifecyclePublisher publisher;
    FO4HDR_CHECK(!publisher.Read().gameplayReady);
    FO4HDR_CHECK(publisher.Read().resetEpoch == 0);
    publisher.PublishTransition(Event::PreLoadGame, true);
    FO4HDR_CHECK(!publisher.Read().gameplayReady);
    FO4HDR_CHECK(publisher.Read().resetEpoch == 1);
    publisher.PublishTransition(Event::PostLoadGame, false);
    FO4HDR_CHECK(!publisher.Read().gameplayReady);
    publisher.PublishTransition(Event::PostLoadGame, true);
    FO4HDR_CHECK(publisher.Read().gameplayReady);
    FO4HDR_CHECK(publisher.Read().resetEpoch == 1);
    publisher.PublishTransition(Event::PreSaveGame, true);
    FO4HDR_CHECK(publisher.Read().gameplayReady);
    publisher.PublishTransition(Event::NewGame, true);
    FO4HDR_CHECK(!publisher.Read().gameplayReady);
    FO4HDR_CHECK(publisher.Read().resetEpoch == 2);
    publisher.PublishTransition(Event::PostLoadGame, true);
    FO4HDR_CHECK(publisher.Read().gameplayReady);
    publisher.Disable();
    publisher.PublishTransition(Event::PostLoadGame, true);
    FO4HDR_CHECK(!publisher.Read().gameplayReady);
    FO4HDR_CHECK(publisher.Read().resetEpoch == 2);
}

void TestCollectedPublication()
{
    fo4hdr::GameHdrSnapshot unavailable{};
    FO4HDR_CHECK(!fo4hdr::PublishCollectedGameSnapshot(unavailable, 0, 42));
    FO4HDR_CHECK(unavailable.collectorThreadId == 0);
    unavailable.taskCollectionAvailable = true;
    FO4HDR_CHECK(!fo4hdr::PublishCollectedGameSnapshot(unavailable, 0, 0));
    FO4HDR_CHECK(unavailable.collectorFailed);

    fo4hdr::GameHdrSnapshot snapshot{};
    snapshot.messagingAvailable = true;
    snapshot.taskCollectionAvailable = true;
    const auto retiredWorld = snapshot.resourceGeneration;
    fo4hdr::ApplyGameLifecycleTransition(snapshot, fo4hdr::GameLifecycleEvent::PreLoadGame, true, 0);
    const auto currentWorld = snapshot.resourceGeneration;
    FO4HDR_CHECK(!fo4hdr::PublishCollectedGameSnapshot(snapshot, retiredWorld, 99));
    FO4HDR_CHECK(snapshot.collectorThreadId == 0);
    FO4HDR_CHECK(snapshot.observationGeneration == 0);
    FO4HDR_CHECK(!snapshot.collectorFailed);
    FO4HDR_CHECK(fo4hdr::PublishCollectedGameSnapshot(snapshot, currentWorld, 42));
    FO4HDR_CHECK(fo4hdr::PublishCollectedGameSnapshot(snapshot, currentWorld, 42));
    FO4HDR_CHECK(snapshot.collectorThreadId == 42);
    FO4HDR_CHECK(snapshot.observationGeneration == 2);
    FO4HDR_CHECK(!fo4hdr::PublishCollectedGameSnapshot(snapshot, currentWorld, 43));
    FO4HDR_CHECK(snapshot.collectorFailed);
    FO4HDR_CHECK(!snapshot.taskCollectionAvailable);
    FO4HDR_CHECK(snapshot.messagingAvailable);
    FO4HDR_CHECK(!fo4hdr::PublishCollectedGameSnapshot(snapshot, currentWorld, 42));
    FO4HDR_CHECK(snapshot.observationGeneration == 2);
    fo4hdr::ApplyGameLifecycleTransition(snapshot, fo4hdr::GameLifecycleEvent::NewGame, true, 0);
    FO4HDR_CHECK(snapshot.collectorFailed);
    FO4HDR_CHECK(!snapshot.taskCollectionAvailable);
    FO4HDR_CHECK(snapshot.messagingAvailable);
}

void TestCopiedRendererResources()
{
    fo4hdr::GameHdrSnapshot observed{};
    observed.rendererObserved = true;
    observed.rendererInitialized = true;
    observed.renderTargetManagerAvailable = true;
    observed.rendererWindowCount = 1;
    observed.rendererWindows[31] = {17, 23, 31, -10, 20, 1920, 1080, true};
    observed.populatedRenderTargetCount = 1;
    observed.renderTargets[100] = {29, 31, 37, 41, 100, true};
    observed.populatedDepthTargetCount = 13;
    observed.populatedCubeTargetCount = 2;
    fo4hdr::GameSnapshotStore store;
    store.Update([&](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        fo4hdr::CopyObservedRenderer(snapshot, observed);
    });
    observed.rendererWindows[31].windowIdentity = 0;
    const auto copy = store.Read();
    FO4HDR_CHECK(copy.rendererObserved && copy.renderTargetManagerAvailable);
    FO4HDR_CHECK(copy.rendererWindowCount == 1 && copy.rendererWindows[31].windowIdentity == 17);
    FO4HDR_CHECK(copy.rendererWindows[31].width == 1920);
    FO4HDR_CHECK(copy.renderTargets[100].unorderedAccessViewIdentity == 41);
    FO4HDR_CHECK(copy.populatedRenderTargetCount == 1 && copy.populatedDepthTargetCount == 13);
    FO4HDR_CHECK(copy.populatedCubeTargetCount == 2);
    FO4HDR_CHECK(copy.renderWidth == 0 && copy.dynamicWidthRatio == 0.0F);
    store.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        fo4hdr::ApplyGameLifecycleTransition(snapshot, fo4hdr::GameLifecycleEvent::PreLoadGame, true, 0);
    });
    const auto retired = store.Read();
    FO4HDR_CHECK(!retired.rendererObserved && !retired.renderTargetManagerAvailable);
    FO4HDR_CHECK(retired.rendererWindowCount == 0 && retired.rendererWindows[31].windowIdentity == 0);
    FO4HDR_CHECK(retired.renderTargets[100].textureIdentity == 0);
    FO4HDR_CHECK(retired.populatedDepthTargetCount == 0 && retired.populatedCubeTargetCount == 0);
}

void TestCopiedImageSpaceAndDrawScope()
{
    std::array<std::byte, 48> bytes{};
    std::array<std::size_t, 12> offsets{};
    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        offsets[index] = index * sizeof(float);
        const float value = static_cast<float>(index + 1);
        std::memcpy(bytes.data() + offsets[index], &value, sizeof(value));
    }
    fo4hdr::ImageSpaceActiveData active{};
    FO4HDR_CHECK(fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, active));
    FO4HDR_CHECK(active.eyeAdaptSpeed == 1.0F && active.cinematicContrast == 12.0F);
    offsets.back() = bytes.size();
    FO4HDR_CHECK(!fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, active));
    FO4HDR_CHECK(active.cinematicContrast == 12.0F);
    offsets.back() = bytes.size() - sizeof(float);
    const float invalid = std::numeric_limits<float>::infinity();
    std::memcpy(bytes.data(), &invalid, sizeof(invalid));
    FO4HDR_CHECK(!fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, active));

    fo4hdr::NativeLutObservation observed{};
    observed.threadId = ::GetCurrentThreadId();
    observed.resourceGeneration = 7;
    observed.validSlots = 1;
    fo4hdr::ResetNativeLutObservation(7);
    observed.generation = fo4hdr::NativeLutObservationGeneration();
    fo4hdr::NativeLutObservation copy{};
    {
        const fo4hdr::NativeLutScope scope(observed);
        FO4HDR_CHECK(fo4hdr::ReadCurrentNativeLutObservation(7, copy));
        FO4HDR_CHECK(copy.serial != 0 && copy.validSlots == 1);
        {
            const fo4hdr::NativeLutScope nested(observed);
            FO4HDR_CHECK(!fo4hdr::ReadCurrentNativeLutObservation(7, copy));
        }
        FO4HDR_CHECK(fo4hdr::ReadCurrentNativeLutObservation(7, copy));
        fo4hdr::ResetNativeLutObservation(8);
        FO4HDR_CHECK(!fo4hdr::ReadCurrentNativeLutObservation(7, copy));
    }
    FO4HDR_CHECK(!fo4hdr::ReadCurrentNativeLutObservation(8, copy));
    fo4hdr::ResetNativeLutObservation(0);
}


}  // namespace

namespace fo4hdr::tests {

void RunGameSnapshotStoreTests()
{
    TestInitialSnapshot();
    TestOwnedReadCopy();
    TestReplacementGeneration();
    TestCoherentConcurrentReads();
    TestConcurrentWriters();
    TestLifecycleOutcome();
    TestLifecycleTransitions();
    TestGraphicsLifecyclePublication();
    TestCollectedPublication();
    TestCopiedImageSpaceAndDrawScope();
    TestCopiedRendererResources();
}

}  // namespace fo4hdr::tests
