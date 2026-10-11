// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include <d3d11.h>

namespace fo4hdr {
struct DrawObservation;

void ClearWholePublicBuffer(ID3D11DeviceContext* context) noexcept;

// Ensure the current epoch before its first native writer; repeated calls preserve content.
// Completed original native content uses whole-frame G22/BT.709 code values.
bool ArmSharedSdrFrame(ID3D11DeviceContext* context) noexcept;
bool SharedSdrFrameHasNativeWriter(ID3D11DeviceContext* context) noexcept;
// Retires the same-frame original LUT reference before native resource reset/resize.
void ResetSharedSdrFrame() noexcept;
void ObserveSharedSdrDrawEntry(ID3D11DeviceContext* context) noexcept;
void ObserveSharedSdrDraw(ID3D11DeviceContext* context,
    const DrawObservation& observation) noexcept;
void ObserveSharedSdrCopy(ID3D11Resource* destination) noexcept;
void ObserveSharedSdrClear(ID3D11RenderTargetView* target) noexcept;
bool SharedSdrFrameReady() noexcept;
float SharedSdrPaperWhite() noexcept;
bool TraceSharedSdrOutput(bool rendered, bool outputWritten, bool startupWritten) noexcept;
void CompleteSharedSdrFrame(bool rendered) noexcept;
void ReportSharedSdr(GraphicsDiagnosticSink sink);

} // namespace fo4hdr
