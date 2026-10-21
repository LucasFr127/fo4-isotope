// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <dxgi.h>

namespace fo4hdr {

struct SwapChainRecord;

// A hook written into this chain's shadow table, or a table installed over it,
// runs before final HDR composition, which then overwrites what it drew.
[[nodiscard]] bool ForeignPresentHookAbove(
    const SwapChainRecord& record, IDXGISwapChain* swapChain) noexcept;

ULONG STDMETHODCALLTYPE HookSwapChainRelease(IDXGISwapChain* swapChain) noexcept;

HRESULT STDMETHODCALLTYPE HookResizeBuffers(
    IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) noexcept;

HRESULT STDMETHODCALLTYPE HookPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept;

} // namespace fo4hdr
