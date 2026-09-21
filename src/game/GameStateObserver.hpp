// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace F4SE { class LoadInterface; }
namespace fo4hdr { struct GameHdrSnapshot; }

namespace fo4hdr::game {

[[nodiscard]] bool ReadRendererSnapshot(GameHdrSnapshot& destination) noexcept;

void InitializeGameStateObserver(const F4SE::LoadInterface* f4se,
    bool gradingObservationRequested = true) noexcept;

}  // namespace fo4hdr::game
