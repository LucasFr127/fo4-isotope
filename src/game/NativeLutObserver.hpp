// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/output/RenderPipelineContract.hpp"

namespace fo4hdr::game {
[[nodiscard]] bool InitializeNativeLutObserver(bool requested) noexcept;
// The pass's Render slot group carries engine pass scopes: tone map and LUT,
// temporal, depth of field, or FXAA.
[[nodiscard]] bool NativeImageSpacePassInstalled(NativeImageSpacePass pass) noexcept;
}
