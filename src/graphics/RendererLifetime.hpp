// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include "Fallout4HDR/output/HdrActivationPolicy.hpp"
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
extern std::atomic<bool> g_gameplayReadyApplied;
extern std::atomic<bool> g_rendererCreationClaimed;

std::uint64_t PublishLifecycleGeneration() noexcept;
void ResetSceneTracking() noexcept;
void RetireLostDeviceGeneration() noexcept;
void BeginPublishedHeartbeatGeneration() noexcept;
bool DisplayGenerationChanged(IDXGISwapChain*, bool& sdrColorSpaceEstablished) noexcept;
void ResetResources(bool retainColorControl = false) noexcept;
bool EnsureResources(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
bool EnsureWideGamutResources(IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) noexcept;
WideGamutResourceReadiness CurrentWideGamutReadiness() noexcept;

} // namespace fo4hdr
