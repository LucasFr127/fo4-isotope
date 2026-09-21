// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeActiveOwner.hpp"

#include <cstddef>
#include <new>

namespace fo4hdr {
namespace {
std::atomic<bool> performanceRequested{};
std::atomic<bool> performanceActive{};
std::atomic<bool> performancePathSelected{};
std::atomic<bool> traceRequested{}, referenceRequested{}, overlayRequested{};
std::atomic<std::uint64_t> tracedFrames{}, tracedHdrFrames{};
}

void SetNativeDiagnosticRequest(const PluginConfiguration& configuration) noexcept
{
    referenceRequested.store(configuration.enabled && configuration.developerMode &&
        configuration.developerHdr10Reference, std::memory_order_relaxed);
    overlayRequested.store(configuration.developerMode && configuration.showDeveloperOverlay,
        std::memory_order_relaxed);
    traceRequested.store(configuration.developerMode && configuration.traceRenderPipeline,
        std::memory_order_release);
}

void RecordNativeFrameTrace(bool hdrCompleted) noexcept
{
    if (!traceRequested.load(std::memory_order_acquire)) return;
    tracedFrames.fetch_add(1, std::memory_order_relaxed);
    if (hdrCompleted) tracedHdrFrames.fetch_add(1, std::memory_order_relaxed);
}

NativeDiagnosticProjection ReadNativeDiagnosticProjection() noexcept
{
    return {traceRequested.load(std::memory_order_acquire),
        referenceRequested.load(std::memory_order_relaxed),
        overlayRequested.load(std::memory_order_relaxed),
        tracedFrames.load(std::memory_order_relaxed), tracedHdrFrames.load(std::memory_order_relaxed)};
}

void SetNativePerformanceRequest(bool requested) noexcept
{
    performancePathSelected.store(false, std::memory_order_relaxed);
    performanceActive.store(false, std::memory_order_relaxed);
    performanceRequested.store(requested, std::memory_order_release);
}

void PublishNativePerformancePath(bool active) noexcept
{
    performanceActive.store(active, std::memory_order_relaxed);
    performancePathSelected.store(true, std::memory_order_release);
}

NativePerformanceProjection ReadNativePerformanceProjection() noexcept
{
    const auto requested = performanceRequested.load(std::memory_order_acquire);
    const auto selected = performancePathSelected.load(std::memory_order_acquire);
    const auto active = performanceActive.load(std::memory_order_relaxed);
    return {requested, selected && active, selected && !active};
}


NativeActiveOwner& ActiveNativeOwner() noexcept
{
    alignas(NativeActiveOwner) static std::byte storage[sizeof(NativeActiveOwner)];
    static auto* owner = ::new (storage) NativeActiveOwner;
    return *owner;
}

bool NativeActiveOwner::Matches(ID3D11Device* candidateDevice, ID3D11DeviceContext* candidateContext,
    IDXGISwapChain* candidateChain, std::uint64_t generation) const noexcept
{
    return device == candidateDevice && context == candidateContext && chain == candidateChain &&
        nativeGeneration == generation;
}

bool ClaimNativeActiveOwner(ID3D11Device* device, ID3D11DeviceContext* context,
    IDXGISwapChain* chain, std::uint64_t generation) noexcept
{
    auto& owner = ActiveNativeOwner();
    if (!device || !context) return false;
    if (!chain) return false;
    if (generation == 0 || owner.revoked.load(std::memory_order_acquire)) return false;
    if (owner.nativeGeneration != 0)
        return owner.Matches(device, context, chain, generation);
    owner.device = device;
    owner.context = context;
    owner.chain = chain;
    owner.nativeGeneration = generation;
    return true;
}

bool NativeActiveOwnerMatches(ID3D11DeviceContext* context, std::uint64_t generation) noexcept
{
    const auto& owner = ActiveNativeOwner();
    return context != nullptr && generation != 0 && !owner.revoked.load(std::memory_order_acquire) &&
        owner.context == context && owner.nativeGeneration == generation;
}

void RevokeNativeActiveOwner() noexcept
{
    ActiveNativeOwner().revoked.store(true, std::memory_order_release);
}


}  // namespace fo4hdr
