// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

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

[[nodiscard]] constexpr MenuCode ClassifyMenu(std::string_view name) noexcept
{
    constexpr std::array<std::string_view, 19> names{
        "UnknownMenu", "HUDMenu", "PipboyMenu", "PowerArmorHUDMenu", "PauseMenu",
        "LoadingMenu", "MainMenu", "DialogueMenu", "WorkshopMenu", "TerminalMenu",
        "BarterMenu", "ContainerMenu", "CursorMenu", "Console", "ScopeMenu",
        "VATSMenu", "FavoritesMenu", "MessageBoxMenu", "LockpickingMenu"};
    for (std::size_t index = 1; index < names.size(); ++index)
    {
        if (names[index] == name)
        {
            return static_cast<MenuCode>(index);
        }
    }
    return MenuCode::Unknown;
}

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
    // Calls must have one serialized producer; the name is borrowed only here.
    [[nodiscard]] std::uint64_t Push(
        std::string_view name, bool opening, std::uint32_t sourceThread) noexcept
    {
        const auto sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto payload = (static_cast<std::uint64_t>(sourceThread) << 9) |
            (static_cast<std::uint64_t>(opening) << 8) |
            static_cast<std::uint8_t>(ClassifyMenu(name));
        auto& slot = slots_[sequence % Capacity];
        // Version and payload share one order, so a reader rejects torn overwrites.
        slot.sequence.store(0, std::memory_order_seq_cst);
        slot.payload.store(payload, std::memory_order_seq_cst);
        slot.sequence.store(sequence, std::memory_order_seq_cst);
        published_.store(sequence, std::memory_order_release);
        return sequence;
    }

    [[nodiscard]] std::uint64_t PublishedSequence() const noexcept
    {
        return published_.load(std::memory_order_acquire);
    }

    // One consumer owns drained_. Failure can mean a racing slot, not an empty stream.
    [[nodiscard]] bool TryPop(MenuEventRecord& destination, std::uint64_t& dropped,
        std::uint64_t maximumSequence = (std::numeric_limits<std::uint64_t>::max)()) noexcept
    {
        dropped = 0;
        const auto published = (std::min)(PublishedSequence(), maximumSequence);
        const auto oldest = published > Capacity ? published - Capacity + 1 : 1;
        const auto caughtUpTo = drained_ + 1 < oldest ? oldest - 1 : drained_;
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
        destination = {sequence, static_cast<std::uint32_t>(payload >> 9),
            static_cast<std::uint8_t>(payload & 0xFF), ((payload >> 8) & 1) != 0};
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

    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> nextSequence_{};
    std::atomic<std::uint64_t> published_{};
    std::uint64_t drained_{};
};

template <std::size_t Capacity>
void DrainMenuEvents(GameHdrSnapshot& snapshot, MenuEventQueue<Capacity>& queue) noexcept
{
    const auto cutoff = queue.PublishedSequence();
    for (std::size_t count = 0; count < Capacity; ++count)
    {
        MenuEventRecord event{};
        std::uint64_t dropped{};
        if (!queue.TryPop(event, dropped, cutoff))
        {
            break;
        }
        snapshot.lastMenuSequence = event.sequence;
        snapshot.menuEventsDropped += dropped;
        if (event.sequence > snapshot.menuResetFloor)
        {
            snapshot.lastMenuCode = event.code;
            snapshot.lastMenuOpening = event.opening;
            snapshot.lastMenuSourceThread = event.sourceThread;
            snapshot.menuEventAvailable = true;
        }
    }
    snapshot.menuHistoryCaughtUp = snapshot.lastMenuSequence >= cutoff;
}

}  // namespace fo4hdr
