// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace F4SE { class LoadInterface; }

namespace fo4hdr::game {

void InitializeGameStateObserver(const F4SE::LoadInterface* f4se) noexcept;

}  // namespace fo4hdr::game
