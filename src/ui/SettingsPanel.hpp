// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/config/PluginConfiguration.hpp"

namespace fo4hdr {

struct LiveSettingsSnapshot;

struct SettingsPanelActions
{
    bool request{};
    bool save{};
};

// Call inside a host-owned ImGui window/frame on the live owner's thread.
// The host retains editable state and dispatches request before save; saving
// persists the live owner's accepted request, not an unaccepted slider preview.
[[nodiscard]] SettingsPanelActions DrawSettingsPanel(PluginConfiguration& editable,
    const LiveSettingsSnapshot& snapshot) noexcept;

}  // namespace fo4hdr
