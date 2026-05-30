// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameSnapshotStore.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

namespace {

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

}  // namespace

namespace fo4hdr::tests {

void RunMenuEventQueueTests()
{
    TestBorrowedName();
    TestCutoffAndOverwrite();
    TestOverwrittenCutoff();
    TestCollectionAndReset();
    TestRejectedCollectionKeepsRecords();
}

}  // namespace fo4hdr::tests
