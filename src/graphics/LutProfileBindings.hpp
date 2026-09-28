// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "DrawObservation.hpp"
#include <FO4Isotope/output/NativeLutObservation.hpp>
#include <array>
#include <cstdint>

namespace fo4hdr {

extern std::array<LutSnapshot, 4> g_lutInspectorLuts;
extern std::array<ImageSpaceIdentity, 4> g_lutInspectorImageSpaces;
extern std::uint32_t g_lutInspectorImageSpaceSlots;
extern const char* g_lutInspectorImageSpaceStatus;
extern std::uint64_t g_lutInspectorFrame;
extern std::uint32_t g_lutInspectorValidatedSlots;
extern const char* g_lutInspectorStatus;

ID3D11Buffer* PrepareLutProfileDraw(ID3D11DeviceContext*, const DrawObservation&, ID3D11Buffer*) noexcept;
const char* NativeLutObserverMessage() noexcept;

} // namespace fo4hdr
