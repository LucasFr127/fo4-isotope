// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <Fallout4HDR/output/ArmorUiComposition.hpp>

namespace fo4hdr {

void InvalidateArmorEncodedComposition() noexcept;
void ArmorEncodedBoundary(const ArmorUiScopeToken&, bool entering) noexcept;
void CloseArmorEncodedCompositionForLifecycle() noexcept;

} // namespace fo4hdr
