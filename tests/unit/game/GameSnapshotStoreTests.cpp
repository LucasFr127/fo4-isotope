// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameSnapshotStore.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <barrier>
#include <cstdint>
#include <thread>

namespace {

void TestInitialSnapshot()
{
    const fo4hdr::GameSnapshotStore store;
    const auto snapshot = store.Read();
    FO4HDR_CHECK(snapshot.generation == 0);
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

}  // namespace

namespace fo4hdr::tests {

void RunGameSnapshotStoreTests()
{
    TestInitialSnapshot();
    TestOwnedReadCopy();
    TestReplacementGeneration();
    TestCoherentConcurrentReads();
    TestConcurrentWriters();
}

}  // namespace fo4hdr::tests
