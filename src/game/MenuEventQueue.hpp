// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"
#include <cstdint>
#include <iterator>
#include <string_view>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <cstring>

namespace fo4hdr {

enum class MenuCode : std::uint8_t
{
    Unknown,
    Hud,
    PipBoy,
    PowerArmor,
    Pause,
    Loading,
    Main,
    Dialogue,
    Workshop,
    Terminal,
    Barter,
    Container,
    Cursor,
    Console,
    Scope,
    Vats,
    Favorites,
    MessageBox,
    Lockpicking
};

static_assert(
    static_cast<std::uint64_t>(UiContextFlag::Lockpicking) <
    (std::uint64_t{ 1 } << 20),
    "MenuEventQueue packed context mask needs another bit");

struct MenuDefinition
{
    std::string_view name;
    UiContextFlag flag;
};

inline constexpr MenuDefinition kMenuDefinitions[]{
    {"UnknownMenu", UiContextFlag::None},
    {"HUDMenu", UiContextFlag::Hud},
    {"PipboyMenu", UiContextFlag::PipBoy},
    {"PowerArmorHUDMenu", UiContextFlag::PowerArmor},
    {"PauseMenu", UiContextFlag::Pause},
    {"LoadingMenu", UiContextFlag::Loading},
    {"MainMenu", UiContextFlag::MainMenu},
    {"DialogueMenu", UiContextFlag::Dialogue},
    {"WorkshopMenu", UiContextFlag::Workshop},
    {"TerminalMenu", UiContextFlag::Terminal},
    {"BarterMenu", UiContextFlag::Barter},
    {"ContainerMenu", UiContextFlag::Container},
    {"CursorMenu", UiContextFlag::Cursor},
    {"Console", UiContextFlag::Console},
    {"ScopeMenu", UiContextFlag::Scope},
    {"VATSMenu", UiContextFlag::Vats},
    {"FavoritesMenu", UiContextFlag::Favorites},
    {"MessageBoxMenu", UiContextFlag::MessageBox},
    {"LockpickingMenu", UiContextFlag::Lockpicking},
};

[[nodiscard]] constexpr MenuCode ClassifyMenu(std::string_view name) noexcept
{
    for (std::size_t i = 1; i < std::size(kMenuDefinitions); ++i)
        if (kMenuDefinitions[i].name == name) return static_cast<MenuCode>(i);
    return MenuCode::Unknown;
}

[[nodiscard]] constexpr const MenuDefinition& DefinitionForMenu(MenuCode menu) noexcept
{
    const auto index = static_cast<std::size_t>(menu);
    return kMenuDefinitions[index < std::size(kMenuDefinitions) ? index : 0];
}

[[nodiscard]] constexpr std::string_view MenuName(MenuCode menu) noexcept
{ return DefinitionForMenu(menu).name; }

[[nodiscard]] constexpr UiContextFlag UiFlagForMenu(MenuCode menu) noexcept
{ return DefinitionForMenu(menu).flag; }
[[nodiscard]] constexpr std::uint64_t MenuContextBit(
    MenuCode menu) noexcept
{
    return static_cast<std::uint64_t>(UiFlagForMenu(menu));
}

[[nodiscard]] constexpr UiContextFlag UiContextsFromOpenMask(
    std::uint64_t openMask) noexcept
{
    constexpr auto nonGameplay =
        static_cast<std::uint64_t>(UiContextFlag::Pause) |
        static_cast<std::uint64_t>(UiContextFlag::Loading) |
        static_cast<std::uint64_t>(UiContextFlag::MainMenu);
    if ((openMask & nonGameplay) == 0)
    {
        openMask |= static_cast<std::uint64_t>(UiContextFlag::Gameplay);
    }
    else
    {
        openMask &= ~static_cast<std::uint64_t>(UiContextFlag::Gameplay);
    }
    return static_cast<UiContextFlag>(openMask);
}

}  // namespace fo4hdr

namespace fo4hdr {

struct MenuEventRecord
{
    std::uint64_t sequence{};
    std::uint32_t sourceThread{};
    std::uint8_t code{};
    bool opening{};
};

template <std::size_t Capacity>
class MenuEventQueue
{
    static_assert(Capacity > 0);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    struct PublishedState
    {
        std::uint64_t sequence{};
        std::uint64_t openMask{};
    };

    // The game UI event source serializes its callbacks. Push is deliberately
    // single-producer and lock-free so it can run while that source lock is
    // held. TryPop is single-consumer and is called only from the designated
    // observation-task collector.
    [[nodiscard]] std::uint64_t Push(
        std::uint8_t code,
        bool opening,
        std::uint32_t sourceThread,
        std::uint64_t contextBit = 0) noexcept
    {
        const auto sequence = nextSequence_.fetch_add(
                                  1,
                                  std::memory_order_relaxed) +
                              1;
        auto openMask = Published().openMask;
        if (opening)
        {
            openMask |= contextBit;
        }
        else
        {
            openMask &= ~contextBit;
        }
        auto& slot = slots_[sequence % Capacity];
        const auto payload =
            (static_cast<std::uint64_t>(sourceThread) << 9) |
            (static_cast<std::uint64_t>(opening) << 8) |
            code;
        // One sequentially consistent order across the version and payload
        // makes an overwrite visible either wholly before or wholly after a
        // reader's two version checks.
        slot.sequence.store(0, std::memory_order_seq_cst);
        slot.payload.store(payload, std::memory_order_seq_cst);
        slot.sequence.store(sequence, std::memory_order_seq_cst);
        published_.store(
            (sequence << kContextBits) | (openMask & kContextMask),
            std::memory_order_release);
        return sequence;
    }

    [[nodiscard]] std::uint64_t CurrentOpenMask() const noexcept
    {
        return Published().openMask;
    }

    [[nodiscard]] std::uint64_t PublishedSequence() const noexcept
    {
        return Published().sequence;
    }

    [[nodiscard]] PublishedState Published() const noexcept
    {
        const auto state = published_.load(std::memory_order_acquire);
        return {
            .sequence = state >> kContextBits,
            .openMask = state & kContextMask
        };
    }

    [[nodiscard]] bool TryPop(
        MenuEventRecord& destination,
        std::uint64_t& dropped,
        std::uint64_t maximumSequence =
            (std::numeric_limits<std::uint64_t>::max)()) noexcept
    {
        dropped = 0;
        const auto published = (std::min)(
            Published().sequence,
            maximumSequence);
        const auto oldest = published > Capacity ?
                                published - Capacity + 1 :
                                1;
        const auto caughtUpTo = drained_ + 1 < oldest ?
                                    oldest - 1 :
                                    drained_;
        const auto sequence = caughtUpTo + 1;
        if (sequence > published)
        {
            return false;
        }
        const auto& slot = slots_[sequence % Capacity];
        if (slot.sequence.load(std::memory_order_seq_cst) != sequence)
        {
            return false;
        }
        const auto payload = slot.payload.load(std::memory_order_seq_cst);
        if (slot.sequence.load(std::memory_order_seq_cst) != sequence)
        {
            return false;
        }
        destination.sequence = sequence;
        destination.sourceThread =
            static_cast<std::uint32_t>(payload >> 9);
        destination.opening = ((payload >> 8) & 1) != 0;
        destination.code = static_cast<std::uint8_t>(payload & 0xFF);
        dropped = caughtUpTo - drained_;
        drained_ = sequence;
        return true;
    }

private:
    struct Slot
    {
        std::atomic<std::uint64_t> sequence{};
        std::atomic<std::uint64_t> payload{};
    };

    static constexpr std::uint64_t kContextBits = 20;
    static constexpr std::uint64_t kContextMask =
        (std::uint64_t{ 1 } << kContextBits) - 1;

    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> nextSequence_{};
    std::atomic<std::uint64_t> published_{};
    std::uint64_t drained_{};
};

}  // namespace fo4hdr

namespace fo4hdr {

struct MenuDrainEvidence
{
    std::uint64_t processed{};
    std::uint64_t dropped{};
    std::uint64_t lastSequence{};
    std::uint64_t publishedSequence{};
    std::uint64_t openMask{};
    MenuCode lastCode{MenuCode::Unknown};
    bool lastOpening{};
    bool caughtUp{};
    bool transitionWitnessValid{true};
    MenuTransitionPairSnapshot pausePair{};
    MenuTransitionPairSnapshot pipBoyPair{};
};

[[nodiscard]] constexpr MenuDrainEvidence BeginMenuDrainEvidence(
    const GameHdrSnapshot& snapshot) noexcept
{
    MenuDrainEvidence evidence{};
    evidence.lastSequence = snapshot.lastMenuSequence;
    evidence.transitionWitnessValid = snapshot.menuTransitionWitnessValid;
    evidence.pausePair = snapshot.pauseMenuPair;
    evidence.pipBoyPair = snapshot.pipBoyMenuPair;
    return evidence;
}

[[nodiscard]] constexpr bool IsCompleteMenuTransitionPair(
    const MenuTransitionPairSnapshot& pair) noexcept
{
    return pair.openSequence != 0 &&
           pair.closeSequence > pair.openSequence;
}

constexpr void RecordMenuTransitionPair(
    MenuTransitionPairSnapshot& pair,
    std::uint64_t sequence,
    bool opening) noexcept
{
    if (IsCompleteMenuTransitionPair(pair))
    {
        return;
    }
    if (opening)
    {
        if (pair.openSequence == 0)
        {
            pair.openSequence = sequence;
        }
        return;
    }
    if (pair.openSequence != 0 && sequence > pair.openSequence)
    {
        pair.closeSequence = sequence;
    }
}

constexpr void RecordMenuTransitionWitness(
    MenuDrainEvidence& evidence,
    MenuCode code,
    std::uint64_t sequence,
    bool opening) noexcept
{
    if (code == MenuCode::Pause)
    {
        RecordMenuTransitionPair(
            evidence.pausePair,
            sequence,
            opening);
    }
    else if (code == MenuCode::PipBoy)
    {
        RecordMenuTransitionPair(
            evidence.pipBoyPair,
            sequence,
            opening);
    }
}

constexpr void AccumulateMenuDrainRecord(
    MenuDrainEvidence& evidence,
    const MenuEventRecord& event,
    std::uint64_t dropped) noexcept
{
    ++evidence.processed;
    evidence.dropped += dropped;
    if (dropped != 0) evidence.transitionWitnessValid = false;
    evidence.lastSequence = event.sequence;
    evidence.lastCode = static_cast<MenuCode>(event.code);
    evidence.lastOpening = event.opening;
    RecordMenuTransitionWitness(evidence, evidence.lastCode, event.sequence, event.opening);
}
[[nodiscard]] constexpr bool CanSynchronizeMenuState(
    const GameHdrSnapshot& snapshot,
    bool allowSynchronization,
    bool synchronizationBoundaryReached,
    bool uiRegistered) noexcept
{
    return allowSynchronization &&
           synchronizationBoundaryReached &&
           uiRegistered &&
           snapshot.lastMenuSequence != 0 &&
           snapshot.menuHistoryComplete &&
           snapshot.menuHistoryCaughtUp;
}

constexpr void ApplyMenuDrainEvidence(
    GameHdrSnapshot& snapshot,
    const MenuDrainEvidence& evidence) noexcept
{
    snapshot.uiContexts = UiContextsFromOpenMask(evidence.openMask);
    snapshot.menuHistoryCaughtUp = evidence.caughtUp;
    snapshot.lastMenuSequence = evidence.lastSequence;
    snapshot.menuEventsDropped += evidence.dropped;
    snapshot.pauseMenuPair = evidence.pausePair;
    snapshot.pipBoyMenuPair = evidence.pipBoyPair;
    snapshot.menuTransitionWitnessValid =
        snapshot.menuTransitionWitnessValid &&
        evidence.transitionWitnessValid &&
        evidence.dropped == 0;
    snapshot.lifecycleGeneration += evidence.processed;
    if (evidence.processed != 0)
    {
        snapshot.lifecycleEvent = GameLifecycleEvent::MenuChanged;
    }
    if (evidence.dropped != 0)
    {
        const auto available = static_cast<std::uint32_t>(
            snapshot.availableFeatures);
        const auto failed = static_cast<std::uint32_t>(
            snapshot.failedFeatures);
        const auto ui = static_cast<std::uint32_t>(
            GameSnapshotFeature::Ui);
        snapshot.availableFeatures = static_cast<GameSnapshotFeature>(
            available & ~ui);
        snapshot.failedFeatures = static_cast<GameSnapshotFeature>(
            failed | ui);
        snapshot.menuHistoryComplete = false;
        snapshot.menuStateSynchronized = false;
        snapshot.lastMenuName = {};
        snapshot.lastMenuOpening = false;
    }
    else if (!evidence.caughtUp)
    {
        const auto available = static_cast<std::uint32_t>(
            snapshot.availableFeatures);
        const auto ui = static_cast<std::uint32_t>(
            GameSnapshotFeature::Ui);
        snapshot.availableFeatures = static_cast<GameSnapshotFeature>(
            available & ~ui);
        snapshot.menuStateSynchronized = false;
    }
    else if (snapshot.menuHistoryComplete && evidence.processed != 0)
    {
        const auto name = MenuName(evidence.lastCode);
        snapshot.lastMenuName = {};
        std::memcpy(
            snapshot.lastMenuName.data(),
            name.data(),
            name.size());
        snapshot.lastMenuOpening = evidence.lastOpening;
    }
}

}  // namespace fo4hdr
