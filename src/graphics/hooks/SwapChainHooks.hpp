// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <dxgi.h>

namespace fo4hdr {

ULONG STDMETHODCALLTYPE HookSwapChainRelease(IDXGISwapChain* swapChain) noexcept;

HRESULT STDMETHODCALLTYPE HookResizeBuffers(
    IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) noexcept;

HRESULT STDMETHODCALLTYPE HookPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept;

} // namespace fo4hdr
