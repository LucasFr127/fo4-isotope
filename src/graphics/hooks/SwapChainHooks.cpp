// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SwapChainHooks.hpp"

#include "HookRecords.hpp"
#include "../FrameTrace.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LiveSettings.hpp"
#include "../PresentFrame.hpp"
#include "../RendererLifetime.hpp"
#include "../resources/PresentationResources.hpp"

#include <wrl/client.h>
#include <algorithm>

namespace fo4hdr {

using Microsoft::WRL::ComPtr;

static void RetireSwapChainGeneration(SwapChainRecord& owner) noexcept
{
    owner.ownsRendererClaim = false;
    RetireLostDeviceGeneration();
}

ULONG STDMETHODCALLTYPE HookSwapChainRelease(IDXGISwapChain* swapChain) noexcept
{
    return InvokeNoexceptBoundary<ULONG>(1, [=]() {
        auto* record = FindSwapChain(swapChain);
        if (record == nullptr || record->nextRelease == nullptr) return 1UL;
        const auto generation = record->identity.Generation();
        const auto remaining = InvokeCapturedNext(record->nextRelease,
            swapChain);
        if (remaining == 0)
            (void)record->identity.Retire(swapChain, generation);
        return remaining;
    }, []() noexcept { RecordAbiException(); });
}

struct SwapChainResize
{
    IDXGISwapChain* swapChain;
    UINT bufferCount;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT flags;
    SwapChainRecord* record{};

    HRESULT ForwardOriginal() const
    {
        return InvokeCapturedNext(record->nextResizeBuffers, swapChain,
            bufferCount, width, height, format, flags);
    }

    void BeginOwnedResize() const
    {
        g_evidence.resizeCount.fetch_add(1, std::memory_order_relaxed);
        (void)g_lifecycle.BeginResize();
        if (g_presentation.swapChain3) (void)SetColorSpace(false);
        ResetResources(true);
        g_referenceHealthy = true;
    }

    bool HdrResizeRequested() const
    {
        return HdrPathRequested() && record->developerHdrDescriptor;
    }

    bool ResourceRebuildFailed() const
    {
        return record->developerHdrDescriptor && g_referenceHealthy &&
            !EnsureWideGamutResources(swapChain, g_rendererDevice.activeDevice, g_rendererDevice.activeContext);
    }

    void RebuildHdrResources() const
    {
        DXGI_SWAP_CHAIN_DESC actual{};
        if (SUCCEEDED(swapChain->GetDesc(&actual)))
        {
            g_presentation.width = actual.BufferDesc.Width;
            g_presentation.height = actual.BufferDesc.Height;
        }
        if (ResourceRebuildFailed())
        {
            g_referenceHealthy = false;
            RecordFailure(GraphicsObservationFailure::resourceCreationFailed);
        }
    }

    HRESULT ResizeHdr() const
    {
        const auto hdrCount = bufferCount == 0 ? 0 : std::max<UINT>(2, bufferCount);
        const auto result = InvokeCapturedNext(record->nextResizeBuffers, swapChain,
            hdrCount, width, height, DXGI_FORMAT_R10G10B10A2_UNORM, flags);
        if (FAILED(result)) g_referenceHealthy = false;
        if (SUCCEEDED(result)) RebuildHdrResources();
        return result;
    }

    static bool DeviceLost(HRESULT result)
    {
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
    }

    void RecordResizeFailure(HRESULT result) const
    {
        if (DeviceLost(result) && record->ownsRendererClaim)
        {
            RecordFailure(GraphicsObservationFailure::deviceLost);
            RetireSwapChainGeneration(*record);
        }
        else
        {
            g_referenceHealthy = false;
            RecordFailure(GraphicsObservationFailure::referenceRenderFailed);
        }
    }

    HRESULT Complete(HRESULT result) const
    {
        (void)g_lifecycle.CompleteResize(SUCCEEDED(result), false);
        if (FAILED(result))
        {
            RecordResizeFailure(result);
            return result;
        }
        BeginPublishedHeartbeatGeneration();
        return result;
    }

    HRESULT Run()
    {
        record = FindSwapChain(swapChain);
        if (record == nullptr || record->nextResizeBuffers == nullptr) return E_FAIL;
        if (!record->ownsRendererClaim) return ForwardOriginal();
        BeginOwnedResize();
        if (!HdrResizeRequested()) return Complete(ForwardOriginal());
        return Complete(ResizeHdr());
    }
};


HRESULT STDMETHODCALLTYPE HookResizeBuffers(
    IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) noexcept
{
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        return SwapChainResize{swapChain, bufferCount, width, height, format, flags}.Run();
    }, []() noexcept { RecordAbiException(); });
}

HRESULT STDMETHODCALLTYPE HookPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept
{
    return InvokeNoexceptBoundary<HRESULT>(E_FAIL, [=]() {
        auto* record = FindSwapChain(swapChain);
        if (record == nullptr || record->nextPresent == nullptr) return E_FAIL;
        if (!record->ownsRendererClaim)
            return InvokeCapturedNext(record->nextPresent, swapChain, syncInterval, flags);
        PollFrameTraceHotkey(swapChain);
        TraceFramePresent(swapChain, FrameTraceKind::nativePresent, {syncInterval, flags});
        const bool rendered = PreparePresentFrame(swapChain, record->developerHdrDescriptor);
        TraceFramePresent(swapChain, FrameTraceKind::nextPresent, {syncInterval, flags});
        const auto result = InvokeCapturedNext(record->nextPresent, swapChain, syncInterval, flags);
        TraceFramePresent(swapChain, FrameTraceKind::presentResult, {syncInterval, flags, result});
        const bool deviceLost = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
        if (deviceLost)
        {
            RecordFailure(GraphicsObservationFailure::deviceLost);
            if (record->ownsRendererClaim) RetireSwapChainGeneration(*record);
        }
        CompletePresentFrame(rendered, !deviceLost && record->ownsRendererClaim);
        return result;
    }, []() noexcept { RecordAbiException(); });
}

} // namespace fo4hdr
