// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>

namespace fo4hdr {

struct RendererDevice
{
    ID3D11Device* activeDevice{};
    ID3D11DeviceContext* activeContext{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> activeContext1;
};
extern RendererDevice g_rendererDevice;
extern HWND g_outputWindow;

extern PipelineHeartbeat g_heartbeat;
extern HdrRendererLifecycle g_lifecycle;
extern std::uint64_t g_frame;
extern std::atomic<bool> g_referenceHealthy;
extern std::atomic<bool> g_productionHealthy;
extern std::atomic<bool> g_gameplayReadyApplied;
extern std::atomic<bool> g_rendererCreationClaimed;

std::uint64_t PublishLifecycleGeneration() noexcept;
void ResetSceneTracking() noexcept;
void RetireLostDeviceGeneration() noexcept;
void BeginPublishedHeartbeatGeneration() noexcept;
bool DisplayGenerationChanged(IDXGISwapChain*, bool& sdrColorSpaceEstablished) noexcept;
void ResetResources(bool retainColorControl = false, bool retainPresentationBuffer = false) noexcept;
bool EnsureResources(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
bool EnsureWideGamutResources(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
void RecordProductionResourceFailure(IDXGISwapChain*) noexcept;
WideGamutResourceReadiness CurrentWideGamutReadiness() noexcept;

} // namespace fo4hdr
