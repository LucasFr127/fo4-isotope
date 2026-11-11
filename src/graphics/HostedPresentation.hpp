// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>

namespace fo4hdr {

// Hosted HDR, latched per device generation when the host reports HDR at
// creation: the HDR profile and operator on the host's chain, without a facade.
extern std::atomic<bool> g_hostedHdrActive;

// The host's state query and its last answer. The query is another module's
// export, resolved once per device generation; nothing else of the host is kept.
struct HostedPresentationState
{
    using Query = bool (*)(HostHdrOutputState*) noexcept;
    Query query{};
    // False once the host's answer stops matching the latch, so its own shaders draw.
    std::atomic<bool> matches{};
    float paperWhiteNits{};
};
extern HostedPresentationState g_hostedState;

// Latches SDR mode or hosted HDR for a published device generation. The hosted
// request and the host's answer complete the inputs; a failed replacement
// build keeps either latch with substitution disabled.
void PrepareLatchedModes(SdrModeInputs inputs, IDXGISwapChain*, ID3D11Device*) noexcept;
// A rolled-back installation leaves neither mode latched.
void ClearLatchedModes() noexcept;
// Once per Present: follows the host's HDR setting and paper white.
void RefreshHostedState(ID3D11DeviceContext*) noexcept;

// The HDR operator constants serve production HDR and hosted HDR.
[[nodiscard]] bool HdrOperatorActive() noexcept;
// The operator's transport white: the host's paper white under hosted HDR,
// which the host multiplies back, and the UI white otherwise.
[[nodiscard]] float OperatorTransportWhiteNits(double uiPaperWhiteNits) noexcept;

}  // namespace fo4hdr
