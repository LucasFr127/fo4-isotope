// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "HdrRenderer.hpp"
#include "NativeSettingsHost.hpp"
#include "passes/ObservedDraw.hpp"
#include "passes/PresentationPass.hpp"
#include "resources/NativePresentationProxy.hpp"

#include <atomic>
#include <cstdint>

namespace fo4hdr {

// Native records own hook identities; one admitted generation owns GPU/settings state.
// The owner stays resident. Revocation never destroys or joins its borrowed workers
// from a native callback, and it is not recycled for another device generation.
struct NativeActiveOwner
{
    ScenePassRenderer renderer;
    NativeUiCapture uiCapture;
    NativePresentationProxy presentationProxy;
    PresentationPass presentation;
    NativeSettingsHost settingsHost;
    SceneTextureHandle presentationScene;
    std::uint64_t lifecycleEpoch{};
    bool hdrPrepared{};
    bool frameRedirected{};
    bool internalWork{};
    bool resourcesRetired{};

    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* chain{};
    std::uint64_t nativeGeneration{};
    std::atomic<bool> revoked{};

    [[nodiscard]] bool Matches(ID3D11Device* candidateDevice, ID3D11DeviceContext* candidateContext,
        IDXGISwapChain* candidateChain, std::uint64_t generation) const noexcept;
};

struct NativePerformanceProjection
{
    bool requested{};
    bool activePath{};
    bool disabledPath{};
};

struct NativeDiagnosticProjection
{
    bool traceRequested{};
    bool referenceRequested{};
    bool overlayRequested{};
    std::uint64_t tracedFrames{};
    std::uint64_t hdrFrames{};
};

void SetNativeDiagnosticRequest(const PluginConfiguration& configuration) noexcept;
void RecordNativeFrameTrace(bool hdrCompleted) noexcept;
[[nodiscard]] NativeDiagnosticProjection ReadNativeDiagnosticProjection() noexcept;

void SetNativePerformanceRequest(bool requested) noexcept;
// Cold requested path selection is independent of per-frame success or missing layers.
void PublishNativePerformancePath(bool active) noexcept;
[[nodiscard]] NativePerformanceProjection ReadNativePerformanceProjection() noexcept;

[[nodiscard]] NativeActiveOwner& ActiveNativeOwner() noexcept;
// Caller has positively qualified the fresh native tuple before outputs escape.
[[nodiscard]] bool ClaimNativeActiveOwner(ID3D11Device* device, ID3D11DeviceContext* context,
    IDXGISwapChain* chain, std::uint64_t generation) noexcept;
[[nodiscard]] bool NativeActiveOwnerMatches(ID3D11DeviceContext* context,
    std::uint64_t generation) noexcept;
void RevokeNativeActiveOwner() noexcept;

}  // namespace fo4hdr
