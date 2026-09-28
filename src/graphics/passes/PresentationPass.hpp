// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "../OperatorConstants.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <array>
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
    std::uint32_t gameUiBackdropCaptured{};
    std::uint32_t gameUiCaptured{};
    std::uint32_t settingsOverlayPresent{};
    std::array<std::uint32_t, 3> reserved{};
};
static_assert(sizeof(CompositionConstants) == 48);


bool RenderReferenceFrame(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
bool RenderSdrPresentationProxy(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
bool RenderBlackSdrFailureFrame(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*, bool sdrColorSpaceEstablished) noexcept;

} // namespace fo4hdr
