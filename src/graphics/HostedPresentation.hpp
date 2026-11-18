// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>

namespace fo4hdr {

// Hosted HDR: the HDR profile and operator on the host's chain, without a facade.
// Set at device creation, and at the end of Present while the session follows
// the host's HDR setting.
extern std::atomic<bool> g_hostedHdrActive;

// The host's state query and what is kept of its answers. The query is another
// module's export, resolved once per device generation and asked once per Present.
struct HostedPresentationState
{
    using Query = bool (*)(HostHdrOutputState*) noexcept;
    Query query{};
    // Hosted HDR can follow the host's live HDR setting this device generation.
    std::atomic<bool> available{};
    // The device generation's SDR mode inputs, for a later switch.
    SdrModeInputs inputs{};
    // False once the host's answer stops matching hosted HDR and the session cannot
    // follow it this frame, so its own shaders draw.
    std::atomic<bool> matches{};
    // What the host shows now. SDR mode substitutes only while it is SDR, since an
    // SDR image inside the host's HDR would be capped at paper white.
    std::atomic<HostOutput> output{};
    float paperWhiteNits{};
};
extern HostedPresentationState g_hostedState;

// Starts SDR mode or hosted HDR for a published device generation and records
// whether hosted HDR is available to follow the host later. The hosted request
// and the host's answer complete the inputs; a failed replacement build keeps
// either mode with substitution disabled.
void PrepareLatchedModes(SdrModeInputs inputs, IDXGISwapChain*, ID3D11Device*) noexcept;
// A rolled-back installation leaves neither mode latched.
void ClearLatchedModes() noexcept;
// Once per Present in a hosted session: switches between hosted HDR and SDR mode
// with the host's live HDR setting when available, and follows its paper white.
void RefreshHostedState(ID3D11DeviceContext*) noexcept;
// Present thread only: the session can still switch to hosted HDR, since a failed
// build or a missing HDR table holds it.
[[nodiscard]] bool HostedHdrFollowable() noexcept;
// The latched SDR mode reason, or the reason SDR mode yields while the host shows HDR.
[[nodiscard]] SdrModeReason ReportedSdrModeReason() noexcept;

// The HDR operator constants serve production HDR and hosted HDR.
[[nodiscard]] bool HdrOperatorActive() noexcept;
// The operator's transport white: the host's paper white under hosted HDR,
// which the host multiplies back, and the UI white otherwise.
[[nodiscard]] float OperatorTransportWhiteNits(double uiPaperWhiteNits) noexcept;

}  // namespace fo4hdr
