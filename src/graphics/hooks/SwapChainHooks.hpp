// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dxgi.h>

namespace fo4hdr {

class ScenePassRenderer;

// CPU lineage only. The native caller separately qualifies chain, scope and
// captured predecessor ownership; these operations establish no image provenance.
[[nodiscard]] bool CloseNativePresentFrame(ScenePassRenderer& renderer, UINT flags) noexcept;
[[nodiscard]] bool OpenNativePresentFrame(ScenePassRenderer& renderer, UINT flags,
    HRESULT result) noexcept;

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT interval, UINT flags) noexcept;
HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* chain, UINT count, UINT width,
    UINT height, DXGI_FORMAT format, UINT flags) noexcept;

}  // namespace fo4hdr
