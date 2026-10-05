// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "d3d11/PresentationSwapChain.hpp"
#include <wrl/client.h>

namespace fo4hdr::tests::gpu {

HRESULT AcquireFacadeBuffer(IDXGISwapChain* nativeChain, ID3D11Texture2D** buffer) noexcept;
Microsoft::WRL::ComPtr<ID3D11Texture2D> FacadeBuffer(IDXGISwapChain& chain);
Microsoft::WRL::ComPtr<IDXGISwapChain> CreateFacade(IDXGISwapChain& nativeChain);

} // namespace fo4hdr::tests::gpu
