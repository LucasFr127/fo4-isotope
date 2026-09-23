// SPDX-License-Identifier: GPL-3.0-or-later

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
