// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "../OperatorConstants.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

struct CompositionConstants
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mode{1};
    float uiPaperWhiteNits{1.0F};
    float sceneReferenceWhiteNits{kDeveloperReferenceNits};
    float displayPeakNits{1000.0F};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
    std::uint32_t settingsOverlayPresent{};
    std::uint32_t sharedUiEncoded{};
    std::uint32_t assumedContent{};
    float armorPeakUiRelative{};
};
static_assert(sizeof(CompositionConstants) == 48);
static_assert(offsetof(CompositionConstants, reserved0) == 24);
static_assert(offsetof(CompositionConstants, reserved1) == 28);
static_assert(offsetof(CompositionConstants, settingsOverlayPresent) == 32);


enum class PresentationResult { rendered, unavailable, retry, blocked, failed };
struct SharedUiPresentation;

PresentationResult RenderReferenceFrame(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
PresentationResult RenderSdrPresentationProxy(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*,
    const SharedUiPresentation* hdrPresentation = nullptr) noexcept;
PresentationResult RenderPublicSharedUiFrame(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*,
    const SharedUiPresentation&) noexcept;
PresentationResult RenderPublicSharedUiSdrFrame(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*,
    const SharedUiPresentation&) noexcept;
bool RenderBlackPresentationFrame(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;

} // namespace fo4hdr
