// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include <cstdint>
namespace fo4hdr::game {
// Called once during validated F4SE plugin initialization, before rendering.
bool InitializeArmorUiObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept;
}
