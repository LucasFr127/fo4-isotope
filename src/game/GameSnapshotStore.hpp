// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/game/GameHdrSnapshot.hpp"

#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace fo4hdr {

class GameSnapshotStore
{
public:
    [[nodiscard]] GameHdrSnapshot Read() const
    {
        const std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    template <class Function>
    void Update(Function&& function)
    {
        // Keep the locked callback plain-data-only; it must not reenter this store.
        static_assert(std::is_nothrow_invocable_v<Function&, GameHdrSnapshot&>);
        const std::scoped_lock lock(mutex_);
        // Capture before the callback so replacing the value cannot reset generation.
        const auto nextGeneration = snapshot_.generation + 1;
        std::invoke(std::forward<Function>(function), snapshot_);
        snapshot_.generation = nextGeneration;
    }

private:
    mutable std::mutex mutex_;
    GameHdrSnapshot snapshot_{};
};

}  // namespace fo4hdr
