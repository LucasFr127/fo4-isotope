// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/output/ArmorUiComposition.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include <d3d11_1.h>
#include <cstdint>
#include <optional>

namespace fo4hdr {

struct DrawObservation;

struct SharedUiPresentation
{
    LinearColorDomain domain{LinearColorDomain::unknown};
    float white{};
    bool assumed{};
    float armorPeakUiRelative{};
};

struct SharedUiWriteEvidence
{
    std::uint64_t revision{}, uncertainty{};
    bool assumed{};
    std::uint64_t publicLifetime{};
    std::uintptr_t publicResource{};
    std::uint64_t contextGeneration{};
    float white{};
};

struct SharedUiOutput
{
    bool rendered{}, written{}, startup{}, publicContent{};
};

bool EnsureSharedUiResources(ID3D11Device* device) noexcept;
bool InitializeSharedUiSdr(ID3D11DeviceContext* context) noexcept;
void ResetSharedUiResources(bool retainContentProvenance = false) noexcept;
void ResetSharedUiContent() noexcept;
void SharedUiBoundary(const ArmorUiScopeToken& token, bool entering) noexcept;
bool ObserveSharedUiDraw(const DrawObservation& observation) noexcept;
SharedUiWriteEvidence CaptureSharedUiWrite(bool freshScene = false, float white = 0.0F) noexcept;
void PublishSharedUiScene(SharedUiWriteEvidence write = CaptureSharedUiWrite()) noexcept;
void PublishSharedUiPresentation() noexcept;
SharedUiWriteEvidence InvalidateSharedUiPresentation() noexcept;
std::optional<SharedUiPresentation> SharedUiPresentationForOutput() noexcept;
std::optional<SharedUiPresentation> SharedUiEmergencyPresentation() noexcept;
void RetireSharedUiPublicImage() noexcept;
void PublishSharedUiPublicImage(SharedUiPresentation presentation, SharedUiWriteEvidence write) noexcept;
SharedUiWriteEvidence PublishSharedUiConversion(SharedUiPresentation presentation,
    SharedUiWriteEvidence write) noexcept;
bool ObserveSharedUiCopy(ID3D11Resource* destination, ID3D11Resource* source) noexcept;
void ObserveSharedUiClear(ID3D11RenderTargetView* target) noexcept;
bool SharedUiStartupSdr() noexcept;
bool SharedUiStartupAssumed() noexcept;
std::optional<SharedUiPresentation> SharedUiStartupPresentation() noexcept;
float SharedUiStartupPaperWhite() noexcept;
void RefreshSharedUiStartupWhite(SharedUiWriteEvidence cleared) noexcept;
bool SharedUiPauseResourcesReady() noexcept;
bool SharedUiFrameReady() noexcept;
bool SharedUiFrameEncoded() noexcept;
bool SharedUiScopeActive(const ArmorUiScopeToken& token) noexcept;
void RejectSharedUiFrame() noexcept;
float SharedUiPaperWhite() noexcept;
bool ScaleSharedSdrVideo(ID3D11DeviceContext* context, float videoWhite, float uiWhite) noexcept;
void TraceSharedUiOutput(SharedUiOutput output,
    const SharedUiPresentation* presentation = nullptr) noexcept;
void CompleteSharedUiFrame(bool rendered) noexcept;
void ReportSharedUi(GraphicsDiagnosticSink sink);

} // namespace fo4hdr
