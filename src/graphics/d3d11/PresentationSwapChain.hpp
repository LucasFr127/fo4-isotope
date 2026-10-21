// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace fo4hdr {

using AcquirePresentationBuffer = HRESULT (*)(
    IDXGISwapChain* nativeChain, ID3D11Texture2D** buffer) noexcept;
using RetirePresentationBuffer = void (*)(IDXGISwapChain* nativeChain) noexcept;

struct PresentationSwapChainCreation
{
    IDXGISwapChain* nativeChain;
    ID3D11Texture2D* buffer;
    DXGI_SWAP_CHAIN_DESC applicationDescription;
    AcquirePresentationBuffer acquireBuffer;
    RetirePresentationBuffer retireBuffer{};
};

// Windowed flip-discard may select private transport; public buffer access stays DISCARD.
// Buffer zero is a genuine FP16 texture; later swapchain interfaces are unavailable.
// Failed resize retires publication until a successful lower-chain resize/reacquire.
HRESULT CreatePresentationSwapChain(
    const PresentationSwapChainCreation& creation, IDXGISwapChain** result) noexcept;

// The application descriptors that CreatePresentationSwapChain accepts.
[[nodiscard]] bool PresentationFacadeDescriptorSupported(
    const DXGI_SWAP_CHAIN_DESC& description) noexcept;

} // namespace fo4hdr
