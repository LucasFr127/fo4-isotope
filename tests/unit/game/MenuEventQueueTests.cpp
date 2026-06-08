// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameSnapshotStore.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <barrier>
#include <exception>
#include <string_view>
#include <thread>

namespace {

constexpr std::array<std::string_view, 3> patternNames{"HUDMenu", "PauseMenu", "PipboyMenu"};
constexpr std::array patternCodes{
    fo4hdr::MenuCode::Hud, fo4hdr::MenuCode::Pause, fo4hdr::MenuCode::PipBoy};

std::uint32_t PatternThread(std::uint64_t sequence)
{
    return static_cast<std::uint32_t>(sequence) ^ 0xA5A50000U;
}

template <std::size_t Capacity>
void PushPattern(fo4hdr::MenuEventQueue<Capacity>& queue, std::uint32_t sequence)
{
    static_cast<void>(queue.Push(patternNames[sequence % patternNames.size()],
        (sequence & 1) != 0, PatternThread(sequence)));
}

void CheckPatternRecord(const fo4hdr::MenuEventRecord& record)
{
    FO4HDR_CHECK(record.code ==
        static_cast<std::uint8_t>(patternCodes[record.sequence % patternCodes.size()]));
    FO4HDR_CHECK(record.opening == ((record.sequence & 1) != 0));
    FO4HDR_CHECK(record.sourceThread == PatternThread(record.sequence));
}

template <std::size_t Capacity>
bool Collect(fo4hdr::GameSnapshotStore& store, fo4hdr::MenuEventQueue<Capacity>& queue,
    std::uint64_t world, std::uint32_t thread)
{
    bool accepted{};
    store.Update([&](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        accepted = fo4hdr::CollectGameMenuSnapshot(snapshot, queue, world, thread);
    });
    return accepted;
}

void EnableCollection(fo4hdr::GameSnapshotStore& store)
{
    store.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        snapshot.taskCollectionAvailable = true;
    });
}

template <std::size_t Capacity>
void ApplyLifecycle(fo4hdr::GameSnapshotStore& store, fo4hdr::MenuEventQueue<Capacity>& queue,
    fo4hdr::GameLifecycleEvent event, bool succeeded)
{
    store.Update([&](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        fo4hdr::ApplyGameLifecycleTransition(snapshot, event, succeeded, queue.PublishedSequence());
    });
}

void CheckMenuEvidence(const fo4hdr::GameHdrSnapshot& snapshot)
{
    if (snapshot.menuEventAvailable)
    {
        FO4HDR_CHECK(snapshot.lastMenuSequence > snapshot.menuResetFloor);
        CheckPatternRecord({snapshot.lastMenuSequence, snapshot.lastMenuSourceThread,
            snapshot.lastMenuCode, snapshot.lastMenuOpening});
    }
    else
    {
        FO4HDR_CHECK(snapshot.lastMenuSequence <= snapshot.menuResetFloor);
        FO4HDR_CHECK(snapshot.lastMenuCode == 0);
        FO4HDR_CHECK(!snapshot.lastMenuOpening);
        FO4HDR_CHECK(snapshot.lastMenuSourceThread == 0);
    }
    FO4HDR_CHECK(!snapshot.menuHistoryCaughtUp ||
        snapshot.lastMenuSequence >= snapshot.menuResetFloor);
}

void TestBorrowedName()
{
    fo4hdr::MenuEventQueue<4> queue;
    fo4hdr::MenuEventRecord record{};
    std::uint64_t dropped{99};
    FO4HDR_CHECK(!queue.TryPop(record, dropped));
    FO4HDR_CHECK(dropped == 0);
    char name[] = "HUDMenu";
    FO4HDR_CHECK(queue.Push(name, true, 17) == 1);
    name[0] = 'X';
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 1);
    FO4HDR_CHECK(record.code == static_cast<std::uint8_t>(fo4hdr::MenuCode::Hud));
    FO4HDR_CHECK(record.opening);
    FO4HDR_CHECK(record.sourceThread == 17);
    FO4HDR_CHECK(queue.Push("unrecognized", false, 19) == 2);
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.code == static_cast<std::uint8_t>(fo4hdr::MenuCode::Unknown));
    FO4HDR_CHECK(!record.opening);
    FO4HDR_CHECK(record.sourceThread == 19);
}

void TestCutoffAndOverwrite()
{
    fo4hdr::MenuEventQueue<4> queue;
    for (std::uint32_t sequence = 1; sequence <= 6; ++sequence)
    {
        FO4HDR_CHECK(queue.Push("PauseMenu", (sequence & 1) != 0, sequence) == sequence);
    }
    const auto cutoff = queue.PublishedSequence();
    fo4hdr::MenuEventRecord record{};
    std::uint64_t dropped{};
    FO4HDR_CHECK(queue.TryPop(record, dropped, cutoff));
    FO4HDR_CHECK(record.sequence == 3);
    FO4HDR_CHECK(dropped == 2);
    FO4HDR_CHECK(queue.Push("PipboyMenu", true, 7) == 7);
    for (std::uint32_t sequence = 4; sequence <= 6; ++sequence)
    {
        FO4HDR_CHECK(queue.TryPop(record, dropped, cutoff));
        FO4HDR_CHECK(record.sequence == sequence);
        FO4HDR_CHECK(record.sourceThread == sequence);
        FO4HDR_CHECK(record.opening == ((sequence & 1) != 0));
        FO4HDR_CHECK(dropped == 0);
    }
    FO4HDR_CHECK(!queue.TryPop(record, dropped, cutoff));
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 7);
    FO4HDR_CHECK(record.code == static_cast<std::uint8_t>(fo4hdr::MenuCode::PipBoy));
    FO4HDR_CHECK(!queue.TryPop(record, dropped));
}

void TestOverwrittenCutoff()
{
    fo4hdr::MenuEventQueue<2> queue;
    static_cast<void>(queue.Push("HUDMenu", true, 1));
    static_cast<void>(queue.Push("HUDMenu", false, 2));
    const auto cutoff = queue.PublishedSequence();
    static_cast<void>(queue.Push("PauseMenu", true, 3));
    static_cast<void>(queue.Push("PauseMenu", false, 4));
    fo4hdr::MenuEventRecord record{};
    std::uint64_t dropped{99};
    FO4HDR_CHECK(!queue.TryPop(record, dropped, cutoff));
    FO4HDR_CHECK(record.sequence == 0);
    FO4HDR_CHECK(dropped == 0);
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 3);
    FO4HDR_CHECK(dropped == 2);
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 4);
    FO4HDR_CHECK(dropped == 0);
}

void TestCollectionAndReset()
{
    fo4hdr::GameSnapshotStore store;
    fo4hdr::MenuEventQueue<256> queue;
    EnableCollection(store);
    static_cast<void>(queue.Push("HUDMenu", true, 7));
    store.Update([&](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        fo4hdr::ApplyGameLifecycleTransition(snapshot,
            fo4hdr::GameLifecycleEvent::PreLoadGame, true, queue.PublishedSequence());
    });
    FO4HDR_CHECK(!store.Read().menuEventAvailable);
    static_cast<void>(queue.Push("PipboyMenu", true, 8));
    const auto before = store.Read();
    FO4HDR_CHECK(Collect(store, queue, before.resourceGeneration, 42));
    const auto collected = store.Read();
    FO4HDR_CHECK(collected.generation == before.generation + 1);
    FO4HDR_CHECK(collected.observationGeneration == 1);
    FO4HDR_CHECK(collected.menuResetFloor == 1);
    FO4HDR_CHECK(collected.lastMenuSequence == 2);
    FO4HDR_CHECK(collected.menuEventsDropped == 0);
    FO4HDR_CHECK(collected.menuEventAvailable);
    FO4HDR_CHECK(collected.menuHistoryCaughtUp);
    FO4HDR_CHECK(collected.lastMenuCode == static_cast<std::uint8_t>(fo4hdr::MenuCode::PipBoy));
    FO4HDR_CHECK(collected.lastMenuOpening);
    FO4HDR_CHECK(collected.lastMenuSourceThread == 8);

    static_cast<void>(queue.Push("PauseMenu", true, 9));
    store.Update([&](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
        fo4hdr::ApplyGameLifecycleTransition(snapshot,
            fo4hdr::GameLifecycleEvent::NewGame, true, queue.PublishedSequence());
    });
    FO4HDR_CHECK(Collect(store, queue, store.Read().resourceGeneration, 42));
    const auto reset = store.Read();
    FO4HDR_CHECK(reset.lastMenuSequence == 3);
    FO4HDR_CHECK(reset.menuResetFloor == 3);
    FO4HDR_CHECK(!reset.menuEventAvailable);
    FO4HDR_CHECK(reset.lastMenuCode == 0);
    FO4HDR_CHECK(!reset.lastMenuOpening);
    FO4HDR_CHECK(reset.lastMenuSourceThread == 0);
    FO4HDR_CHECK(reset.menuHistoryCaughtUp);
}

void TestRejectedCollectionKeepsRecords()
{
    fo4hdr::GameSnapshotStore store;
    fo4hdr::MenuEventQueue<4> queue;
    EnableCollection(store);
    static_cast<void>(queue.Push("HUDMenu", true, 7));
    FO4HDR_CHECK(!Collect(store, queue, 1, 42));
    FO4HDR_CHECK(store.Read().lastMenuSequence == 0);
    FO4HDR_CHECK(store.Read().collectorThreadId == 0);
    FO4HDR_CHECK(Collect(store, queue, 0, 42));
    FO4HDR_CHECK(store.Read().lastMenuSequence == 1);
    static_cast<void>(queue.Push("HUDMenu", false, 7));
    FO4HDR_CHECK(!Collect(store, queue, 0, 43));
    FO4HDR_CHECK(store.Read().collectorFailed);
    FO4HDR_CHECK(store.Read().lastMenuSequence == 1);
    fo4hdr::MenuEventRecord record{};
    std::uint64_t dropped{};
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 2);
}

void TestConcurrentOverwrite()
{
    constexpr std::uint32_t publications = 8192;
    constexpr std::size_t capacity = 8;
    fo4hdr::MenuEventQueue<capacity> queue;
    std::barrier start{2};
    std::jthread producer([&] {
        for (std::uint32_t sequence = 1; sequence <= publications; ++sequence)
        {
            PushPattern(queue, sequence);
            if (sequence == 2 * capacity)
            {
                start.arrive_and_wait();
            }
        }
    });
    start.arrive_and_wait();
    std::uint64_t previous{};
    std::uint64_t consumed{};
    std::uint64_t totalDropped{};
    const auto consume = [&] {
        fo4hdr::MenuEventRecord record{};
        std::uint64_t dropped{};
        if (queue.TryPop(record, dropped))
        {
            CheckPatternRecord(record);
            FO4HDR_CHECK(record.sequence > previous);
            FO4HDR_CHECK(dropped == record.sequence - previous - 1);
            previous = record.sequence;
            ++consumed;
            totalDropped += dropped;
        }
    };
    for (std::uint32_t attempt = 0; attempt < publications; ++attempt)
    {
        consume();
    }
    producer.join();
    for (std::size_t attempt = 0; attempt < capacity; ++attempt)
    {
        consume();
    }
    FO4HDR_CHECK(previous == publications);
    FO4HDR_CHECK(queue.PublishedSequence() == publications);
    FO4HDR_CHECK(totalDropped >= capacity);
    FO4HDR_CHECK(consumed + totalDropped == publications);
}

void CheckResetSnapshot(const fo4hdr::GameHdrSnapshot& snapshot, std::uint64_t publications,
    std::uint64_t attempts, std::uint64_t accepted)
{
    FO4HDR_CHECK(snapshot.resourceGeneration == snapshot.lifecycleGeneration);
    FO4HDR_CHECK(snapshot.generation == 1 + snapshot.lifecycleGeneration + attempts);
    FO4HDR_CHECK(snapshot.observationGeneration == accepted);
    FO4HDR_CHECK(snapshot.menuResetFloor <= publications);
    FO4HDR_CHECK(snapshot.lastMenuSequence <= publications);
    FO4HDR_CHECK(snapshot.menuEventsDropped <= snapshot.lastMenuSequence);
    FO4HDR_CHECK(snapshot.taskCollectionAvailable);
    FO4HDR_CHECK(!snapshot.collectorFailed);
    CheckMenuEvidence(snapshot);
}

void TestConcurrentResetAndDrain()
{
    constexpr std::uint32_t iterations = 1000;
    fo4hdr::GameSnapshotStore store;
    fo4hdr::MenuEventQueue<8> queue;
    EnableCollection(store);
    std::barrier start{2};
    std::exception_ptr resetFailure;
    std::jthread resetter([&] {
        start.arrive_and_wait();
        try
        {
            for (std::uint32_t reset = 0; reset < iterations; ++reset)
            {
                const auto event = reset % 2 == 0 ? fo4hdr::GameLifecycleEvent::PreLoadGame :
                    fo4hdr::GameLifecycleEvent::NewGame;
                ApplyLifecycle(store, queue, event, true);
            }
        }
        catch (...)
        {
            resetFailure = std::current_exception();
        }
    });
    start.arrive_and_wait();
    std::uint64_t accepted{};
    std::uint64_t previousSequence{};
    std::uint64_t previousDropped{};
    std::uint64_t previousFloor{};
    for (std::uint32_t sequence = 1; sequence <= iterations; ++sequence)
    {
        PushPattern(queue, sequence);
        accepted += Collect(store, queue, store.Read().resourceGeneration, 42);
        const auto snapshot = store.Read();
        CheckResetSnapshot(snapshot, sequence, sequence, accepted);
        FO4HDR_CHECK(snapshot.lastMenuSequence >= previousSequence);
        FO4HDR_CHECK(snapshot.menuEventsDropped >= previousDropped);
        FO4HDR_CHECK(snapshot.menuResetFloor >= previousFloor);
        previousSequence = snapshot.lastMenuSequence;
        previousDropped = snapshot.menuEventsDropped;
        previousFloor = snapshot.menuResetFloor;
    }
    resetter.join();
    FO4HDR_CHECK(resetFailure == nullptr);
    FO4HDR_CHECK(Collect(store, queue, store.Read().resourceGeneration, 42));
    const auto final = store.Read();
    CheckResetSnapshot(final, iterations, iterations + 1, accepted + 1);
    FO4HDR_CHECK(final.resourceGeneration == iterations);
    FO4HDR_CHECK(final.lastMenuSequence == iterations);
    FO4HDR_CHECK(final.menuHistoryCaughtUp);
    FO4HDR_CHECK(final.loading);
}

void TestStaleWorldAfterResetKeepsRecords()
{
    fo4hdr::GameSnapshotStore store;
    fo4hdr::MenuEventQueue<4> queue;
    EnableCollection(store);
    const auto capturedWorld = store.Read().resourceGeneration;
    PushPattern(queue, 1);
    ApplyLifecycle(store, queue, fo4hdr::GameLifecycleEvent::PreLoadGame, true);
    PushPattern(queue, 2);
    const auto before = store.Read();
    FO4HDR_CHECK(!Collect(store, queue, capturedWorld, 42));
    const auto rejected = store.Read();
    FO4HDR_CHECK(rejected.generation == before.generation + 1);
    FO4HDR_CHECK(rejected.lastMenuSequence == before.lastMenuSequence);
    FO4HDR_CHECK(rejected.menuEventsDropped == before.menuEventsDropped);
    FO4HDR_CHECK(rejected.observationGeneration == 0);
    FO4HDR_CHECK(rejected.collectorThreadId == 0);
    FO4HDR_CHECK(rejected.menuResetFloor == 1);
    CheckMenuEvidence(rejected);
    fo4hdr::MenuEventRecord retained{};
    std::uint64_t dropped{};
    FO4HDR_CHECK(queue.TryPop(retained, dropped));
    FO4HDR_CHECK(retained.sequence == 1);
    FO4HDR_CHECK(dropped == 0);
    CheckPatternRecord(retained);
    FO4HDR_CHECK(Collect(store, queue, rejected.resourceGeneration, 42));
    const auto collected = store.Read();
    FO4HDR_CHECK(collected.lastMenuSequence == 2);
    FO4HDR_CHECK(collected.menuResetFloor == 1);
    CheckMenuEvidence(collected);
}

void TestEpochRetirementKeepsAccounting()
{
    using Event = fo4hdr::GameLifecycleEvent;
    fo4hdr::GameSnapshotStore store;
    fo4hdr::MenuEventQueue<4> queue;
    EnableCollection(store);
    for (std::uint32_t sequence = 1; sequence <= 8; ++sequence)
    {
        PushPattern(queue, sequence);
    }
    FO4HDR_CHECK(Collect(store, queue, 0, 42));
    const auto observed = store.Read();
    FO4HDR_CHECK(observed.lastMenuSequence == 8);
    FO4HDR_CHECK(observed.menuEventsDropped == 4);
    FO4HDR_CHECK(observed.menuEventAvailable);
    ApplyLifecycle(store, queue, Event::PreLoadGame, true);
    for (const auto succeeded : {false, true})
    {
        ApplyLifecycle(store, queue, Event::PostLoadGame, succeeded);
        const auto loaded = store.Read();
        FO4HDR_CHECK(loaded.lastMenuSequence == observed.lastMenuSequence);
        FO4HDR_CHECK(loaded.menuEventsDropped == observed.menuEventsDropped);
        FO4HDR_CHECK(loaded.menuResetFloor == 8);
        FO4HDR_CHECK(loaded.loading == !succeeded);
        CheckMenuEvidence(loaded);
    }
    PushPattern(queue, 9);
    ApplyLifecycle(store, queue, Event::NewGame, true);
    const auto reset = store.Read();
    FO4HDR_CHECK(reset.lastMenuSequence == 8);
    FO4HDR_CHECK(reset.menuEventsDropped == 4);
    FO4HDR_CHECK(reset.menuResetFloor == 9);
    FO4HDR_CHECK(!reset.menuHistoryCaughtUp);
    CheckMenuEvidence(reset);
    FO4HDR_CHECK(Collect(store, queue, reset.resourceGeneration, 42));
    const auto drained = store.Read();
    FO4HDR_CHECK(drained.lastMenuSequence == 9);
    FO4HDR_CHECK(drained.menuEventsDropped == 4);
    FO4HDR_CHECK(drained.menuHistoryCaughtUp);
    CheckMenuEvidence(drained);
    PushPattern(queue, 10);
    FO4HDR_CHECK(Collect(store, queue, reset.resourceGeneration, 42));
    const auto renewed = store.Read();
    FO4HDR_CHECK(renewed.lastMenuSequence == 10);
    FO4HDR_CHECK(renewed.menuEventsDropped == 4);
    FO4HDR_CHECK(renewed.menuEventAvailable);
    CheckMenuEvidence(renewed);
    store.Update(fo4hdr::DisableGameSnapshotCollection);
    PushPattern(queue, 11);
    FO4HDR_CHECK(!Collect(store, queue, reset.resourceGeneration, 42));
    const auto failed = store.Read();
    FO4HDR_CHECK(failed.collectorFailed);
    FO4HDR_CHECK(failed.lastMenuSequence == renewed.lastMenuSequence);
    FO4HDR_CHECK(failed.menuEventsDropped == renewed.menuEventsDropped);
    FO4HDR_CHECK(failed.observationGeneration == renewed.observationGeneration);
    FO4HDR_CHECK(failed.menuEventAvailable);
    CheckMenuEvidence(failed);
    fo4hdr::MenuEventRecord retained{};
    std::uint64_t dropped{};
    FO4HDR_CHECK(queue.TryPop(retained, dropped));
    FO4HDR_CHECK(retained.sequence == 11);
    FO4HDR_CHECK(dropped == 0);
    CheckPatternRecord(retained);
}

}  // namespace

namespace fo4hdr::tests {

void RunMenuEventQueueTests()
{
    TestBorrowedName();
    TestCutoffAndOverwrite();
    TestOverwrittenCutoff();
    TestCollectionAndReset();
    TestRejectedCollectionKeepsRecords();
    TestConcurrentOverwrite();
    TestConcurrentResetAndDrain();
    TestStaleWorldAfterResetKeepsRecords();
    TestEpochRetirementKeepsAccounting();
}

}  // namespace fo4hdr::tests
