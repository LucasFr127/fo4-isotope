// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <FO4Isotope/output/ArmorUiComposition.hpp>

namespace fo4hdr {

void InvalidateArmorEncodedComposition() noexcept;
void ArmorEncodedBoundary(const ArmorUiScopeToken&, bool entering) noexcept;
void CloseArmorEncodedCompositionForLifecycle() noexcept;

} // namespace fo4hdr
