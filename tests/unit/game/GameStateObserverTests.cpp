// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"

#include "TestHarness.hpp"

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"
#include "GameSnapshotStore.hpp"
#include "ImageSpaceObserver.hpp"
#include "MenuEventQueue.hpp"
#include "GameStateObserver.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fo4hdr::tests {

void CheckSnapshotFeatures(fo4hdr::GameHdrSnapshot& snapshot)
{
    snapshot.availableFeatures =
        fo4hdr::GameSnapshotFeature::Messaging |
        fo4hdr::GameSnapshotFeature::Renderer;
    FO4HDR_CHECK(
        fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::Messaging));
    FO4HDR_CHECK(
        !fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::Lifecycle));

    snapshot.availableFeatures =
        fo4hdr::GameSnapshotFeature::Messaging |
        fo4hdr::GameSnapshotFeature::Lifecycle |
        fo4hdr::GameSnapshotFeature::ImageSpace |
        fo4hdr::GameSnapshotFeature::Renderer |
        fo4hdr::GameSnapshotFeature::RenderTargets |
        fo4hdr::GameSnapshotFeature::Ui;
    fo4hdr::MarkGameSnapshotFeatureFailed(
        snapshot,
        fo4hdr::GameSnapshotFeature::ImageSpace);
    FO4HDR_CHECK(
        !fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::ImageSpace));
    FO4HDR_CHECK(
        fo4hdr::HasGameSnapshotFeature(
            snapshot.failedFeatures,
            fo4hdr::GameSnapshotFeature::ImageSpace));
    for (const auto unrelated : {
             fo4hdr::GameSnapshotFeature::Messaging,
             fo4hdr::GameSnapshotFeature::Lifecycle,
             fo4hdr::GameSnapshotFeature::Renderer,
             fo4hdr::GameSnapshotFeature::RenderTargets,
             fo4hdr::GameSnapshotFeature::Ui })
    {
        FO4HDR_CHECK(
            fo4hdr::HasGameSnapshotFeature(
                snapshot.availableFeatures,
                unrelated));
        FO4HDR_CHECK(
            !fo4hdr::HasGameSnapshotFeature(
                snapshot.failedFeatures,
                unrelated));
    }
    fo4hdr::MarkGameSnapshotFeatureAvailable(
        snapshot,
        fo4hdr::GameSnapshotFeature::ImageSpace);
    FO4HDR_CHECK(
        fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::ImageSpace));
    FO4HDR_CHECK(
        !fo4hdr::HasGameSnapshotFeature(
            snapshot.failedFeatures,
            fo4hdr::GameSnapshotFeature::ImageSpace));
}

void RunSnapshotTests()
{
    fo4hdr::GameHdrSnapshot snapshot{};
    snapshot.uiContexts =
        fo4hdr::UiContextFlag::Gameplay |
        fo4hdr::UiContextFlag::Hud |
        fo4hdr::UiContextFlag::Subtitles;

    FO4HDR_CHECK(
        fo4hdr::HasUiContext(
            snapshot.uiContexts,
            fo4hdr::UiContextFlag::Hud));
    FO4HDR_CHECK(
        fo4hdr::HasUiContext(
            snapshot.uiContexts,
            fo4hdr::UiContextFlag::Subtitles));
    FO4HDR_CHECK(
        !fo4hdr::HasUiContext(
            snapshot.uiContexts,
            fo4hdr::UiContextFlag::Loading));

    snapshot.activeLutCount = 4;
    FO4HDR_CHECK(fo4hdr::HasValidLutCount(snapshot));
    snapshot.activeLutCount = 5;
    FO4HDR_CHECK(!fo4hdr::HasValidLutCount(snapshot));

    CheckSnapshotFeatures(snapshot);
}

fo4hdr::GameHdrSnapshot PopulatedLifecycleSnapshot()
{
    fo4hdr::GameHdrSnapshot snapshot{};
    snapshot.availableFeatures =
        fo4hdr::GameSnapshotFeature::Messaging |
        fo4hdr::GameSnapshotFeature::Lifecycle |
        fo4hdr::GameSnapshotFeature::ImageSpace |
        fo4hdr::GameSnapshotFeature::Renderer |
        fo4hdr::GameSnapshotFeature::RenderTargets |
        fo4hdr::GameSnapshotFeature::Ui;
    snapshot.failedFeatures = fo4hdr::GameSnapshotFeature::ImageSpace;
    snapshot.resourceGeneration = 7;
    snapshot.lifecycleGeneration = 11;
    snapshot.observationGeneration = 13;
    snapshot.rendererDeviceIdentity = 0x1000;
    snapshot.rendererContextIdentity = 0x2000;
    snapshot.imageSpaceIdentity = 0x3000;
    snapshot.imageSpaceEffectCount = 4;
    snapshot.imageSpaceEffects[0].present = true;
    snapshot.rendererWindows[0].present = true;
    snapshot.renderTargets[0].resourcePresent = true;
    snapshot.luts[0].engineTextureId = 0x4000;
    snapshot.activeLutCount = 1;
    snapshot.uiContexts = fo4hdr::UiContextFlag::PipBoy;
    snapshot.menuStateSynchronized = true;
    snapshot.lastMenuSequence = 42;
    snapshot.menuEventsDropped = 3;

    return snapshot;
}

void CheckPreLoadInvalidation(const fo4hdr::GameHdrSnapshot& snapshot)
{
    FO4HDR_CHECK(snapshot.loading);
    FO4HDR_CHECK(snapshot.resourceGeneration == 8);
    FO4HDR_CHECK(snapshot.lifecycleGeneration == 12);
    FO4HDR_CHECK(snapshot.observationGeneration == 13);
    FO4HDR_CHECK(snapshot.lifecycleEvent ==
                 fo4hdr::GameLifecycleEvent::PreLoadGame);
    FO4HDR_CHECK(fo4hdr::HasGameSnapshotFeature(
        snapshot.availableFeatures,
        fo4hdr::GameSnapshotFeature::Messaging));
    FO4HDR_CHECK(fo4hdr::HasGameSnapshotFeature(
        snapshot.availableFeatures,
        fo4hdr::GameSnapshotFeature::Lifecycle));
    for (const auto feature : {
             fo4hdr::GameSnapshotFeature::ImageSpace,
             fo4hdr::GameSnapshotFeature::Renderer,
             fo4hdr::GameSnapshotFeature::RenderTargets,
             fo4hdr::GameSnapshotFeature::Ui })
    {
        FO4HDR_CHECK(!fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            feature));
        FO4HDR_CHECK(!fo4hdr::HasGameSnapshotFeature(
            snapshot.failedFeatures,
            feature));
    }
    FO4HDR_CHECK(snapshot.rendererDeviceIdentity == 0);
    FO4HDR_CHECK(snapshot.rendererContextIdentity == 0);
    FO4HDR_CHECK(snapshot.imageSpaceIdentity == 0);
    FO4HDR_CHECK(snapshot.imageSpaceEffectCount == 0);
    FO4HDR_CHECK(!snapshot.imageSpaceEffects[0].present);
    FO4HDR_CHECK(!snapshot.rendererWindows[0].present);
    FO4HDR_CHECK(!snapshot.renderTargets[0].resourcePresent);
    FO4HDR_CHECK(snapshot.luts[0].engineTextureId == 0);
    FO4HDR_CHECK(snapshot.activeLutCount == 0);
    FO4HDR_CHECK(snapshot.uiContexts == fo4hdr::UiContextFlag::None);
    FO4HDR_CHECK(!snapshot.menuStateSynchronized);
    FO4HDR_CHECK(snapshot.lastMenuSequence == 42);
    FO4HDR_CHECK(snapshot.menuEventsDropped == 3);

}

void RunSnapshotLifecycleInvalidationTests()
{
    auto snapshot = PopulatedLifecycleSnapshot();
    fo4hdr::ApplyGameLifecycleTransition(
        snapshot,
        fo4hdr::GameLifecycleEvent::PreLoadGame,
        true);

    CheckPreLoadInvalidation(snapshot);
    fo4hdr::ApplyGameLifecycleTransition(
        snapshot,
        fo4hdr::GameLifecycleEvent::PostLoadGame,
        false);
    FO4HDR_CHECK(snapshot.loading);
    fo4hdr::ApplyGameLifecycleTransition(
        snapshot,
        fo4hdr::GameLifecycleEvent::PostLoadGame,
        true);
    FO4HDR_CHECK(!snapshot.loading);

    snapshot.rendererDeviceIdentity = 0x5000;
    fo4hdr::MarkGameSnapshotFeatureAvailable(
        snapshot,
        fo4hdr::GameSnapshotFeature::Renderer);
    fo4hdr::ApplyGameLifecycleTransition(
        snapshot,
        fo4hdr::GameLifecycleEvent::NewGame,
        true);
    FO4HDR_CHECK(!snapshot.loading);
    FO4HDR_CHECK(snapshot.resourceGeneration == 9);
    FO4HDR_CHECK(snapshot.rendererDeviceIdentity == 0);
    FO4HDR_CHECK(!fo4hdr::HasGameSnapshotFeature(
        snapshot.availableFeatures,
        fo4hdr::GameSnapshotFeature::Renderer));
}

void RunImageSpaceSemanticDataTests()
{
    constexpr std::array<std::size_t, 12> offsets{
        128, 132, 136, 140, 144, 148,
        152, 156, 160, 164, 168, 172
    };
    std::array<std::byte, 176> bytes{};
    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        const auto value = static_cast<float>(index + 1) * 1.25F;
        std::memcpy(bytes.data() + offsets[index], &value, sizeof(value));
    }

    fo4hdr::ImageSpaceActiveData decoded{};
    FO4HDR_CHECK(fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, decoded));
    FO4HDR_CHECK(decoded.eyeAdaptSpeed == 1.25F);
    FO4HDR_CHECK(decoded.bloomBlurRadius == 2.5F);
    FO4HDR_CHECK(decoded.bloomThreshold == 3.75F);
    FO4HDR_CHECK(decoded.bloomScale == 5.0F);
    FO4HDR_CHECK(decoded.receiveBloomThreshold == 6.25F);
    FO4HDR_CHECK(decoded.white == 7.5F);
    FO4HDR_CHECK(decoded.sunlightScale == 8.75F);
    FO4HDR_CHECK(decoded.skyScale == 10.0F);
    FO4HDR_CHECK(decoded.eyeAdaptStrength == 11.25F);
    FO4HDR_CHECK(decoded.cinematicSaturation == 12.5F);
    FO4HDR_CHECK(decoded.cinematicBrightness == 13.75F);
    FO4HDR_CHECK(decoded.cinematicContrast == 15.0F);

    for (const auto offset : offsets)
    {
        auto invalid = bytes;
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(invalid.data() + offset, &nan, sizeof(nan));
        FO4HDR_CHECK(
            !fo4hdr::DecodeImageSpaceActiveData(invalid, offsets, decoded));
    }
    FO4HDR_CHECK(
        !fo4hdr::DecodeImageSpaceActiveData(
            std::span<const std::byte>{bytes.data(), bytes.size() - 1},
            offsets,
            decoded));

    FO4HDR_CHECK(fo4hdr::ValidateImageSpaceEffectListPrefix(
        {72, 69, 72, 0x1000}, 72));
    FO4HDR_CHECK(fo4hdr::ValidateImageSpaceEffectListPrefix(
        {73, 69, 72, 0x1000}, 72));
    FO4HDR_CHECK(fo4hdr::ValidateImageSpaceEffectListPrefix(
        {231, 69, 72, 0x1000}, 72));
    FO4HDR_CHECK(!fo4hdr::ValidateImageSpaceEffectListPrefix(
        {71, 69, 71, 0x1000}, 72));
    FO4HDR_CHECK(!fo4hdr::ValidateImageSpaceEffectListPrefix(
        {231, 69, 71, 0x1000}, 72));
    FO4HDR_CHECK(!fo4hdr::ValidateImageSpaceEffectListPrefix(
        {231, 73, 72, 0x1000}, 72));
    FO4HDR_CHECK(!fo4hdr::ValidateImageSpaceEffectListPrefix(
        {72, 73, 72, 0x1000}, 72));
    FO4HDR_CHECK(!fo4hdr::ValidateImageSpaceEffectListPrefix(
        {72, 69, 73, 0x1000}, 72));
    FO4HDR_CHECK(!fo4hdr::ValidateImageSpaceEffectListPrefix(
        {72, 69, 72, 0}, 72));
}

bool WaitForRaceStart(const std::atomic<bool>& start, const std::atomic<bool>& cancel) noexcept
{
    while (!start.load(std::memory_order_acquire) && !cancel.load(std::memory_order_acquire))
        std::this_thread::yield();
    return !cancel.load(std::memory_order_acquire);
}
class SnapshotPublicationRace
{
    static constexpr std::uint64_t iterations = 20000;
    static constexpr std::uint64_t correlationMask = 0x9E3779B97F4A7C15ULL;
    fo4hdr::GameSnapshotStore store;
    std::atomic<bool> start{};
    std::atomic<bool> done{};
    std::atomic<std::uint32_t> readersReady{};
    std::atomic<std::uint32_t> readersStarted{};
    std::atomic<std::uint32_t> readersObservedUpdate{};
    std::atomic<std::uint64_t> totalReads{};
    std::atomic<std::uint32_t> raceFailures{};
    std::vector<std::thread> readers;

    static bool Correlated(const fo4hdr::GameHdrSnapshot& value)
    {
        return value.frameId == value.generation &&
            value.resourceGeneration == (value.generation ^ correlationMask) &&
            value.renderWidth == static_cast<std::uint32_t>(value.generation) &&
            value.renderHeight == ~static_cast<std::uint32_t>(value.generation) &&
            value.activeImageSpace.eyeAdaptStrength == static_cast<float>(value.generation) &&
            fo4hdr::HasValidLutCount(value);
    }

    void ReadUpdates()
    {
        std::uint64_t previousGeneration = 0;
        bool observedUpdate = false;
        ++readersStarted;
        do
        {
            const auto value = store.Read();
            ++totalReads;
            if (value.generation < previousGeneration) ++raceFailures;
            previousGeneration = value.generation;
            if (value.generation != 0 && !Correlated(value)) ++raceFailures;
            const bool intermediate = value.generation != 0 && value.generation < iterations;
            if (!observedUpdate && intermediate)
            {
                observedUpdate = true;
                ++readersObservedUpdate;
            }
        } while (!done.load(std::memory_order_acquire));
    }

    void Read()
    {
        ++readersReady;
        if (!WaitForRaceStart(start, done)) return;
        ReadUpdates();
    }

    void StopReaders()
    {
        done.store(true, std::memory_order_release);
        for (auto& reader : readers) reader.join();
    }

    bool StartReaders()
    {
        for (std::size_t readerIndex = 0; readerIndex < 4; ++readerIndex)
            readers.emplace_back([this]() { Read(); });
        const auto allReadersReady = fo4hdr::test::WaitUntil([&]() {
            return readersReady.load(std::memory_order_acquire) == readers.size();
        });
        FO4HDR_CHECK(allReadersReady);
        if (!allReadersReady) return false;
        start.store(true, std::memory_order_release);
        const auto readersStartedInTime = fo4hdr::test::WaitUntil([&]() {
            return readersStarted.load(std::memory_order_acquire) == readers.size();
        });
        FO4HDR_CHECK(readersStartedInTime);
        return readersStartedInTime;
    }

    void Publish(std::uint64_t generation)
    {
        fo4hdr::GameHdrSnapshot value{};
        value.frameId = generation;
        value.resourceGeneration = generation ^ correlationMask;
        value.renderWidth = static_cast<std::uint32_t>(generation);
        value.renderHeight = ~static_cast<std::uint32_t>(generation);
        value.activeImageSpace.eyeAdaptStrength = static_cast<float>(generation);
        value.activeLutCount = static_cast<std::uint32_t>(generation % 5);
        store.Update([&value](fo4hdr::GameHdrSnapshot& destination) noexcept { destination = value; });
    }

    bool WriteUpdates()
    {
        bool readersObservedFirstUpdate = true;
        for (std::uint64_t generation = 1; generation <= iterations; ++generation)
        {
            Publish(generation);
            if (generation == 1)
            {
                readersObservedFirstUpdate = fo4hdr::test::WaitUntil([&]() {
                    return readersObservedUpdate.load(std::memory_order_acquire) == readers.size();
                });
                if (!readersObservedFirstUpdate) break;
            }
        }
        return readersObservedFirstUpdate;
    }

    void CheckFinal()
    {
        const auto final = store.Read();
        FO4HDR_CHECK(raceFailures.load() == 0);
        FO4HDR_CHECK(totalReads.load() >= readers.size());
        FO4HDR_CHECK(readersObservedUpdate.load() == readers.size());
        FO4HDR_CHECK(final.generation == iterations);
        FO4HDR_CHECK(final.frameId == iterations);
        FO4HDR_CHECK(final.resourceGeneration == (iterations ^ correlationMask));
    }
public:
    void Run()
    {
        if (!StartReaders()) { StopReaders(); return; }
        const auto readersObservedFirstUpdate = WriteUpdates();
        StopReaders();
        FO4HDR_CHECK(readersObservedFirstUpdate);
        if (!readersObservedFirstUpdate) return;
        CheckFinal();
    }
};

void RunSnapshotPublicationRaceTests()
{
    SnapshotPublicationRace race;
    race.Run();
}
void RunSnapshotConcurrentWritersTests()
{
    constexpr std::uint64_t writerCount = 4;
    constexpr std::uint64_t updatesPerWriter = 4000;
    fo4hdr::GameSnapshotStore store;
    std::atomic<bool> start{};
    std::atomic<bool> cancel{};
    std::atomic<std::uint32_t> ready{};
    std::vector<std::thread> writers;
    for (std::uint64_t index = 0; index < writerCount; ++index)
    {
        writers.emplace_back([&store, &start, &cancel, &ready]() {
            ++ready;
            if (!WaitForRaceStart(start, cancel))
            {
                return;
            }
            for (std::uint64_t update = 0;
                 update < updatesPerWriter;
                 ++update)
            {
                store.Update(
                    [](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                        ++snapshot.frameId;
                        ++snapshot.observationGeneration;
                    });
            }
        });
    }
    const auto writersReady = fo4hdr::test::WaitUntil([&]() {
        return ready.load(std::memory_order_acquire) == writerCount;
    });
    FO4HDR_CHECK(writersReady);
    if (!writersReady)
    {
        cancel.store(true, std::memory_order_release);
    }
    start.store(true, std::memory_order_release);
    for (auto& writer : writers)
    {
        writer.join();
    }
    if (!writersReady)
    {
        return;
    }

    const auto snapshot = store.Read();
    constexpr auto expected = writerCount * updatesPerWriter;
    FO4HDR_CHECK(snapshot.generation == expected);
    FO4HDR_CHECK(snapshot.frameId == expected);
    FO4HDR_CHECK(snapshot.observationGeneration == expected);
}

void RunMenuEventQueueTests()
{
    fo4hdr::MenuEventQueue<3> queue;
    fo4hdr::MenuEventRecord event{};
    std::uint64_t dropped{};

    const auto pipboyBit = fo4hdr::MenuContextBit(
        fo4hdr::MenuCode::PipBoy);
    FO4HDR_CHECK(queue.Push(1, true, 101, pipboyBit) == 1);
    FO4HDR_CHECK(queue.Published().openMask == pipboyBit);
    FO4HDR_CHECK(queue.Push(0, false, 202) == 2);
    FO4HDR_CHECK(queue.TryPop(event, dropped));
    FO4HDR_CHECK(dropped == 0);
    FO4HDR_CHECK(event.sequence == 1);
    FO4HDR_CHECK(event.code == 1);
    FO4HDR_CHECK(event.opening);
    FO4HDR_CHECK(event.sourceThread == 101);
    FO4HDR_CHECK(queue.TryPop(event, dropped));
    FO4HDR_CHECK(event.sequence == 2);
    FO4HDR_CHECK(event.code == 0);
    FO4HDR_CHECK(!event.opening);
    FO4HDR_CHECK(event.sourceThread == 202);
    FO4HDR_CHECK(!queue.TryPop(event, dropped));

    FO4HDR_CHECK(queue.Push(3, true, 303) == 3);
    FO4HDR_CHECK(queue.Push(4, false, 404) == 4);
    FO4HDR_CHECK(queue.Push(5, true, 505) == 5);
    FO4HDR_CHECK(queue.Push(6, false, 606) == 6);
    FO4HDR_CHECK(queue.TryPop(event, dropped));
    FO4HDR_CHECK(dropped == 1);
    FO4HDR_CHECK(event.sequence == 4);
    FO4HDR_CHECK(event.code == 4);
    FO4HDR_CHECK(!event.opening);
    FO4HDR_CHECK(event.sourceThread == 404);
}

void CheckMenuNames()
{
    using Pair = std::pair<std::string_view, fo4hdr::MenuCode>;
    constexpr std::array cases{
        Pair{ "HUDMenu", fo4hdr::MenuCode::Hud },
        Pair{ "PipboyMenu", fo4hdr::MenuCode::PipBoy },
        Pair{ "PowerArmorHUDMenu", fo4hdr::MenuCode::PowerArmor },
        Pair{ "PauseMenu", fo4hdr::MenuCode::Pause },
        Pair{ "LoadingMenu", fo4hdr::MenuCode::Loading },
        Pair{ "MainMenu", fo4hdr::MenuCode::Main },
        Pair{ "DialogueMenu", fo4hdr::MenuCode::Dialogue },
        Pair{ "WorkshopMenu", fo4hdr::MenuCode::Workshop },
        Pair{ "TerminalMenu", fo4hdr::MenuCode::Terminal },
        Pair{ "BarterMenu", fo4hdr::MenuCode::Barter },
        Pair{ "ContainerMenu", fo4hdr::MenuCode::Container },
        Pair{ "CursorMenu", fo4hdr::MenuCode::Cursor },
        Pair{ "Console", fo4hdr::MenuCode::Console },
        Pair{ "ScopeMenu", fo4hdr::MenuCode::Scope },
        Pair{ "VATSMenu", fo4hdr::MenuCode::Vats },
        Pair{ "FavoritesMenu", fo4hdr::MenuCode::Favorites },
        Pair{ "MessageBoxMenu", fo4hdr::MenuCode::MessageBox },
        Pair{ "LockpickingMenu", fo4hdr::MenuCode::Lockpicking }
    };
    for (const auto& [name, code] : cases)
    {
        FO4HDR_CHECK(fo4hdr::ClassifyMenu(name) == code);
        FO4HDR_CHECK(fo4hdr::MenuName(code) == name);
    }
    FO4HDR_CHECK(
        fo4hdr::ClassifyMenu("pipboyMenu") == fo4hdr::MenuCode::Unknown);
    FO4HDR_CHECK(
        fo4hdr::ClassifyMenu("PipboyMenuX") == fo4hdr::MenuCode::Unknown);
    FO4HDR_CHECK(
        fo4hdr::ClassifyMenu("PipboyMen") == fo4hdr::MenuCode::Unknown);

}

void CheckMenuCutoff()
{
    fo4hdr::MenuEventRecord event{};
    std::uint64_t dropped{};
    fo4hdr::MenuEventQueue<8> cutoffQueue;
    FO4HDR_CHECK(cutoffQueue.Push(1, true, 1) == 1);
    FO4HDR_CHECK(cutoffQueue.Push(2, true, 2) == 2);
    const auto cutoff = cutoffQueue.PublishedSequence();
    FO4HDR_CHECK(cutoffQueue.Push(3, true, 3) == 3);
    FO4HDR_CHECK(cutoffQueue.TryPop(event, dropped, cutoff));
    FO4HDR_CHECK(event.sequence == 1);
    FO4HDR_CHECK(cutoffQueue.TryPop(event, dropped, cutoff));
    FO4HDR_CHECK(event.sequence == 2);
    FO4HDR_CHECK(!cutoffQueue.TryPop(event, dropped, cutoff));
    FO4HDR_CHECK(cutoffQueue.TryPop(event, dropped));
    FO4HDR_CHECK(event.sequence == 3);
}

void RunMenuContextAndOverflowEvidenceTests()
{
    CheckMenuNames();
    fo4hdr::MenuEventQueue<3> overflow;
    const auto pauseBit = fo4hdr::MenuContextBit(fo4hdr::MenuCode::Pause);
    FO4HDR_CHECK(overflow.Push(1, true, 1, pauseBit) == 1);
    FO4HDR_CHECK(overflow.Push(0, true, 2) == 2);
    FO4HDR_CHECK(overflow.Push(0, false, 3) == 3);
    FO4HDR_CHECK(overflow.Push(1, false, 4, pauseBit) == 4);
    FO4HDR_CHECK(overflow.Published().openMask == 0);
    fo4hdr::MenuEventRecord event{};
    std::uint64_t dropped{};
    FO4HDR_CHECK(overflow.TryPop(event, dropped));
    FO4HDR_CHECK(dropped == 1);
    FO4HDR_CHECK(event.sequence == 2);
    FO4HDR_CHECK(
        fo4hdr::HasUiContext(
            fo4hdr::UiContextsFromOpenMask(
                overflow.Published().openMask),
            fo4hdr::UiContextFlag::Gameplay));

    fo4hdr::MenuEventQueue<64> observedSession;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        FO4HDR_CHECK(
            observedSession.Push(0, false, index + 1) == index + 1);
        FO4HDR_CHECK(observedSession.TryPop(event, dropped));
        FO4HDR_CHECK(dropped == 0);
    }
    for (std::uint32_t index = 0; index < 97; ++index)
    {
        FO4HDR_CHECK(
            observedSession.Push(0, false, index + 5) == index + 5);
    }
    FO4HDR_CHECK(observedSession.TryPop(event, dropped));
    FO4HDR_CHECK(dropped == 33);
    FO4HDR_CHECK(event.sequence == 38);

    fo4hdr::MenuEventQueue<256> correctedSession;
    for (std::uint32_t index = 0; index < 105; ++index)
    {
        FO4HDR_CHECK(
            correctedSession.Push(0, false, index + 1) == index + 1);
    }
    std::uint32_t drained{};
    while (correctedSession.TryPop(event, dropped))
    {
        FO4HDR_CHECK(dropped == 0);
        ++drained;
    }
    FO4HDR_CHECK(drained == 105);

    CheckMenuCutoff();
}

void RunGameObservationPolicyTests()
{
    using Event = fo4hdr::GameLifecycleEvent;
    constexpr std::array events{
        Event::None,
        Event::PostLoad,
        Event::PostPostLoad,
        Event::PreLoadGame,
        Event::PostLoadGame,
        Event::PreSaveGame,
        Event::PostSaveGame,
        Event::DeleteGame,
        Event::InputLoaded,
        Event::NewGame,
        Event::GameLoaded,
        Event::GameDataReady,
        Event::MenuChanged
    };
    for (const auto event : events)
    {
        for (const bool succeeded : { false, true })
        {
            const auto expected =
                (event == Event::PostLoadGame && succeeded) ||
                event == Event::PreSaveGame ||
                event == Event::NewGame;
            FO4HDR_CHECK(
                fo4hdr::ShouldCollectGameSnapshotAtLifecycle(
                    event,
                    succeeded) == expected);
        }
    }
}

void CheckWitnessedMenuTransitions()
{
    fo4hdr::MenuEventQueue<32> witnessQueue;
    for (std::uint64_t sequence = 1; sequence <= 23; ++sequence)
    {
        auto code = fo4hdr::MenuCode::Unknown;
        bool opening{};
        if (sequence == 18 || sequence == 19)
        {
            code = fo4hdr::MenuCode::Pause;
            opening = sequence == 18;
        }
        else if (sequence == 20 || sequence == 22)
        {
            code = fo4hdr::MenuCode::PipBoy;
            opening = sequence == 20;
        }
        FO4HDR_CHECK(witnessQueue.Push(
            static_cast<std::uint8_t>(code),
            opening,
            1,
            fo4hdr::MenuContextBit(code)) == sequence);
    }
    fo4hdr::MenuDrainEvidence witnesses{};
    fo4hdr::MenuEventRecord witnessEvent{};
    std::uint64_t witnessDropped{};
    while (witnessQueue.TryPop(witnessEvent, witnessDropped))
    {
        fo4hdr::AccumulateMenuDrainRecord(
            witnesses,
            witnessEvent,
            witnessDropped);
    }
    witnesses.publishedSequence = 23;
    witnesses.openMask = witnessQueue.Published().openMask;
    witnesses.caughtUp = true;
    fo4hdr::GameHdrSnapshot witnessed{};
    fo4hdr::ApplyMenuDrainEvidence(witnessed, witnesses);
    FO4HDR_CHECK(witnessed.menuTransitionWitnessValid);
    FO4HDR_CHECK(fo4hdr::IsCompleteMenuTransitionPair(
        witnessed.pauseMenuPair));
    FO4HDR_CHECK(fo4hdr::IsCompleteMenuTransitionPair(
        witnessed.pipBoyMenuPair));
    FO4HDR_CHECK(witnessed.pauseMenuPair.openSequence == 18);
    FO4HDR_CHECK(witnessed.pauseMenuPair.closeSequence == 19);
    FO4HDR_CHECK(witnessed.pipBoyMenuPair.openSequence == 20);
    FO4HDR_CHECK(witnessed.pipBoyMenuPair.closeSequence == 22);

}

void CheckSplitMenuTransitions()
{
    fo4hdr::MenuTransitionPairSnapshot split{};
    fo4hdr::RecordMenuTransitionPair(split, 29, false);
    FO4HDR_CHECK(split.openSequence == 0);
    FO4HDR_CHECK(split.closeSequence == 0);
    fo4hdr::RecordMenuTransitionPair(split, 30, true);
    fo4hdr::RecordMenuTransitionPair(split, 31, true);
    FO4HDR_CHECK(split.openSequence == 30);
    FO4HDR_CHECK(split.closeSequence == 0);
    fo4hdr::RecordMenuTransitionPair(split, 32, false);
    FO4HDR_CHECK(fo4hdr::IsCompleteMenuTransitionPair(split));
    fo4hdr::RecordMenuTransitionPair(split, 33, true);
    FO4HDR_CHECK(split.openSequence == 30);
    FO4HDR_CHECK(split.closeSequence == 32);

    fo4hdr::GameHdrSnapshot splitSnapshot{};
    auto splitOpen = fo4hdr::BeginMenuDrainEvidence(splitSnapshot);
    fo4hdr::AccumulateMenuDrainRecord(
        splitOpen,
        {.sequence = 50, .code = static_cast<std::uint8_t>(fo4hdr::MenuCode::Pause), .opening = true},
        0);
    splitOpen.caughtUp = true;
    fo4hdr::ApplyMenuDrainEvidence(splitSnapshot, splitOpen);
    auto splitClose = fo4hdr::BeginMenuDrainEvidence(splitSnapshot);
    fo4hdr::AccumulateMenuDrainRecord(
        splitClose,
        {.sequence = 51, .code = static_cast<std::uint8_t>(fo4hdr::MenuCode::Pause), .opening = false},
        0);
    splitClose.caughtUp = true;
    fo4hdr::ApplyMenuDrainEvidence(splitSnapshot, splitClose);
    FO4HDR_CHECK(fo4hdr::IsCompleteMenuTransitionPair(
        splitSnapshot.pauseMenuPair));
    FO4HDR_CHECK(splitSnapshot.pauseMenuPair.openSequence == 50);
    FO4HDR_CHECK(splitSnapshot.pauseMenuPair.closeSequence == 51);

    auto invalidateWitness = fo4hdr::BeginMenuDrainEvidence(splitSnapshot);
    fo4hdr::AccumulateMenuDrainRecord(
        invalidateWitness,
        {.sequence = 53, .code = static_cast<std::uint8_t>(fo4hdr::MenuCode::Unknown), .opening = false},
        1);
    invalidateWitness.caughtUp = true;
    fo4hdr::ApplyMenuDrainEvidence(splitSnapshot, invalidateWitness);
    FO4HDR_CHECK(!splitSnapshot.menuTransitionWitnessValid);
    auto cleanAfterInvalid = fo4hdr::BeginMenuDrainEvidence(splitSnapshot);
    fo4hdr::AccumulateMenuDrainRecord(
        cleanAfterInvalid,
        {.sequence = 54, .code = static_cast<std::uint8_t>(fo4hdr::MenuCode::Pause), .opening = true},
        0);
    cleanAfterInvalid.caughtUp = true;
    fo4hdr::ApplyMenuDrainEvidence(splitSnapshot, cleanAfterInvalid);
    FO4HDR_CHECK(!splitSnapshot.menuTransitionWitnessValid);
    FO4HDR_CHECK(splitSnapshot.pauseMenuPair.openSequence == 50);
    FO4HDR_CHECK(splitSnapshot.pauseMenuPair.closeSequence == 51);

}

void CheckMenuCatchUp(const fo4hdr::GameHdrSnapshot& snapshot)
{
    fo4hdr::GameHdrSnapshot racing{};
    racing.availableFeatures = fo4hdr::GameSnapshotFeature::Ui;
    racing.menuStateSynchronized = true;
    const fo4hdr::MenuDrainEvidence notCaughtUp{
        .processed = 1,
        .lastSequence = 40,
        .publishedSequence = 41,
        .caughtUp = false
    };
    fo4hdr::ApplyMenuDrainEvidence(racing, notCaughtUp);
    FO4HDR_CHECK(!racing.menuHistoryCaughtUp);
    FO4HDR_CHECK(!racing.menuStateSynchronized);
    FO4HDR_CHECK(!fo4hdr::HasGameSnapshotFeature(
        racing.availableFeatures,
        fo4hdr::GameSnapshotFeature::Ui));
    FO4HDR_CHECK(!fo4hdr::HasGameSnapshotFeature(
        racing.failedFeatures,
        fo4hdr::GameSnapshotFeature::Ui));
    FO4HDR_CHECK(!fo4hdr::CanSynchronizeMenuState(
        racing,
        true,
        true,
        true));

    const fo4hdr::MenuDrainEvidence caughtUp{
        .processed = 1,
        .lastSequence = 41,
        .publishedSequence = 41,
        .caughtUp = true
    };
    fo4hdr::ApplyMenuDrainEvidence(racing, caughtUp);
    FO4HDR_CHECK(fo4hdr::CanSynchronizeMenuState(
        racing,
        true,
        true,
        true));
    FO4HDR_CHECK(!fo4hdr::CanSynchronizeMenuState(
        racing,
        false,
        true,
        true));
    FO4HDR_CHECK(!fo4hdr::CanSynchronizeMenuState(
        snapshot,
        true,
        true,
        true));
}

void RunMenuDrainEvidenceTests()
{
    fo4hdr::GameHdrSnapshot snapshot{};
    snapshot.availableFeatures = fo4hdr::GameSnapshotFeature::Ui;
    snapshot.menuStateSynchronized = true;
    std::memcpy(
        snapshot.lastMenuName.data(),
        "PauseMenu",
        sizeof("PauseMenu"));

    const fo4hdr::MenuDrainEvidence overflow{
        .processed = 3,
        .dropped = 2,
        .lastSequence = 9,
        .publishedSequence = 9,
        .openMask = fo4hdr::MenuContextBit(fo4hdr::MenuCode::PipBoy),
        .lastCode = fo4hdr::MenuCode::PipBoy,
        .lastOpening = true,
        .caughtUp = true
    };
    fo4hdr::ApplyMenuDrainEvidence(snapshot, overflow);
    FO4HDR_CHECK(!snapshot.menuHistoryComplete);
    FO4HDR_CHECK(snapshot.menuHistoryCaughtUp);
    FO4HDR_CHECK(!snapshot.menuStateSynchronized);
    FO4HDR_CHECK(snapshot.menuEventsDropped == 2);
    FO4HDR_CHECK(!snapshot.menuTransitionWitnessValid);
    FO4HDR_CHECK(snapshot.lastMenuSequence == 9);
    FO4HDR_CHECK(snapshot.lastMenuName[0] == '\0');
    FO4HDR_CHECK(
        !fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::Ui));
    FO4HDR_CHECK(
        fo4hdr::HasGameSnapshotFeature(
            snapshot.failedFeatures,
            fo4hdr::GameSnapshotFeature::Ui));
    FO4HDR_CHECK(
        fo4hdr::HasUiContext(
            snapshot.uiContexts,
            fo4hdr::UiContextFlag::PipBoy));

    const fo4hdr::MenuDrainEvidence recoveredCurrentState{
        .processed = 1,
        .lastSequence = 10,
        .publishedSequence = 10,
        .openMask = 0,
        .lastCode = fo4hdr::MenuCode::PipBoy,
        .lastOpening = false,
        .caughtUp = true
    };
    fo4hdr::ApplyMenuDrainEvidence(snapshot, recoveredCurrentState);
    FO4HDR_CHECK(!snapshot.menuHistoryComplete);
    FO4HDR_CHECK(snapshot.menuEventsDropped == 2);
    FO4HDR_CHECK(!snapshot.menuTransitionWitnessValid);
    FO4HDR_CHECK(snapshot.lastMenuName[0] == '\0');
    FO4HDR_CHECK(
        fo4hdr::HasUiContext(
            snapshot.uiContexts,
            fo4hdr::UiContextFlag::Gameplay));

    CheckWitnessedMenuTransitions();
    CheckSplitMenuTransitions();
    CheckMenuCatchUp(snapshot);
}

void RunOwnedTaskTransferTests()
{
    struct Probe
    {
        explicit Probe(std::uint32_t& destructions) noexcept :
            destructions_(&destructions)
        {}
        ~Probe() { ++*destructions_; }
        std::uint32_t* destructions_{};
    };

    std::uint32_t successDestructions{};
    Probe* queued{};
    FO4HDR_CHECK(fo4hdr::TransferOwnedTask(
        new Probe(successDestructions),
        [&queued](Probe* task) { queued = task; }));
    FO4HDR_CHECK(queued != nullptr);
    FO4HDR_CHECK(successDestructions == 0);
    delete queued;
    FO4HDR_CHECK(successDestructions == 1);

    std::uint32_t failureDestructions{};
    FO4HDR_CHECK(!fo4hdr::TransferOwnedTask(
        new Probe(failureDestructions),
        [](Probe*) { throw std::runtime_error("enqueue failed"); }));
    FO4HDR_CHECK(failureDestructions == 1);
}

class MenuQueueOverwriteRace
{
    static constexpr std::uint32_t iterations = 50000;
    fo4hdr::MenuEventQueue<2> queue;
    std::atomic<bool> readerStarted{};
    std::atomic<bool> writerDone{};
    std::atomic<std::uint64_t> readCount{};
    std::atomic<std::uint64_t> dropCount{};
    std::atomic<std::uint32_t> failures{};

    static bool Correlated(const fo4hdr::MenuEventRecord& event) noexcept
    {
        return event.sourceThread == event.sequence &&
            event.code == static_cast<std::uint8_t>(event.sequence) &&
            event.opening == ((event.sequence & 1) != 0);
    }
    void Validate(const fo4hdr::MenuEventRecord& event, std::uint64_t dropped) noexcept
    {
        dropCount.fetch_add(dropped, std::memory_order_relaxed);
        ++readCount;
        if (!Correlated(event)) ++failures;
    }
    void Read()
    {
        readerStarted.store(true, std::memory_order_release);
        fo4hdr::MenuEventRecord event{};
        std::uint64_t dropped{};
        while (!writerDone.load(std::memory_order_acquire))
        {
            if (queue.TryPop(event, dropped)) Validate(event, dropped);
            else std::this_thread::yield();
        }
        while (queue.TryPop(event, dropped)) Validate(event, dropped);
    }
    bool Write()
    {
        bool firstReadObserved = true;
        for (std::uint32_t sequence = 1; sequence <= iterations; ++sequence)
        {
            [[maybe_unused]] const auto published = queue.Push(
                static_cast<std::uint8_t>(sequence), (sequence & 1) != 0, sequence,
                fo4hdr::MenuContextBit(fo4hdr::MenuCode::Pause));
            if (sequence == 1)
            {
                firstReadObserved = fo4hdr::test::WaitUntil([&]() {
                    return readCount.load(std::memory_order_acquire) != 0;
                });
                if (!firstReadObserved) break;
            }
        }
        return firstReadObserved;
    }
    void CheckFinal()
    {
        FO4HDR_CHECK(readCount.load() > 1);
        FO4HDR_CHECK(failures.load() == 0);
        FO4HDR_CHECK(readCount.load() + dropCount.load() == iterations);
        FO4HDR_CHECK(queue.Published().openMask == 0);
    }
public:
    void Run()
    {
        std::thread reader([&]() { Read(); });
        const auto stopReader = [&]() {
            writerDone.store(true, std::memory_order_release);
            reader.join();
        };
        const auto readerStartedInTime = fo4hdr::test::WaitUntil([&]() {
            return readerStarted.load(std::memory_order_acquire);
        });
        FO4HDR_CHECK(readerStartedInTime);
        if (!readerStartedInTime) { stopReader(); return; }
        const auto firstReadObserved = Write();
        stopReader();
        FO4HDR_CHECK(firstReadObserved);
        if (!firstReadObserved) return;
        CheckFinal();
    }
};

void RunMenuEventQueueOverwriteRaceTests()
{
    MenuQueueOverwriteRace race;
    race.Run();
}

}  // namespace fo4hdr::tests