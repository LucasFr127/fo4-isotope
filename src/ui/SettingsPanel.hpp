// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "Fallout4HDR/config/PluginConfiguration.hpp"

namespace fo4hdr {

struct SettingsWindowView;

struct SettingsPanelActions
{
    bool request{};
    bool save{};
};

// Call inside a host-owned ImGui window/frame on the live owner's thread.
// The host retains editable state and dispatches request before save; saving
// persists the live owner's accepted request, not an unaccepted slider preview.
[[nodiscard]] SettingsPanelActions DrawSettingsPanel(HdrSettings& editable,
    const SettingsWindowView& snapshot) noexcept;

}  // namespace fo4hdr
