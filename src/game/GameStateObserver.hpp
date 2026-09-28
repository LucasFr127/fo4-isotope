// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/game/GameHdrSnapshot.hpp"
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace fo4hdr::game {

// Optional plain-data diagnostics invoked from the F4SE lifecycle callback.
// Implementations must not throw, retain the snapshot address, or access game
// objects through it.
using LifecycleDiagnosticsCallback =
    void (*)(const GameHdrSnapshot& snapshot) noexcept;

// Registers F4SE lifecycle/UI observation and window-thread settings input
// interaction after exact-runtime validation. Failure leaves the validated
// plugin loaded with HDR disabled.
[[nodiscard]] bool InitializeGameStateObserver(
    const void* messagingInterface,
    const void* taskInterface,
    std::uint32_t pluginHandle,
    LifecycleDiagnosticsCallback diagnostics = nullptr) noexcept;

}  // namespace fo4hdr::game

namespace fo4hdr {

// enqueue must take ownership on normal return and leave ownership here if
// it throws. Returns true after transfer; otherwise destroys a non-null
// task locally and returns false.
template <class Task, class Enqueue>
[[nodiscard]] bool TransferOwnedTask(
    Task* task,
    Enqueue&& enqueue) noexcept
{
    if (task == nullptr)
    {
        return false;
    }
    try
    {
        std::invoke(std::forward<Enqueue>(enqueue), task);
        return true;
    }
    catch (...)
    {
        delete task;
        return false;
    }
}

}  // namespace fo4hdr

namespace fo4hdr { class GameSnapshotStore; }
namespace fo4hdr::game::observation {
void ObserveRendererAndTargets(GameSnapshotStore& store, std::string_view trigger);
}  // namespace fo4hdr::game::observation
