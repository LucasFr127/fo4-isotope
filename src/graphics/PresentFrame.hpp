// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace fo4hdr {

void ApplyGraphicsLifecycleBeforeDraw(ID3D11DeviceContext* context) noexcept;
[[nodiscard]] bool PreparePresentFrame(IDXGISwapChain*, bool developerHdrDescriptor);
void CompletePresentFrame(bool rendered, bool commitSettings);


struct GpuPerformanceQuerySlot;
[[nodiscard]] bool PerformanceResolutionMatches(IDXGISwapChain* swapChain, bool measurePerformance) noexcept;
[[nodiscard]] bool EnsureGpuPerformanceQueries(ID3D11Device* device, bool measurePerformance) noexcept;
[[nodiscard]] bool CpuPerformanceComplete() noexcept;
void ResetPerformanceMeasurement() noexcept;
void CompleteCpuPerformanceSample(LARGE_INTEGER started) noexcept;
void ClearGpuPerformanceQueries() noexcept;
void PollGpuPerformanceQueries(ID3D11DeviceContext* context) noexcept;

class ScopedGpuPerformanceSample
{
public:
    explicit ScopedGpuPerformanceSample(ID3D11DeviceContext* context) noexcept;
    ~ScopedGpuPerformanceSample() noexcept;
    ScopedGpuPerformanceSample(const ScopedGpuPerformanceSample&) = delete;
    ScopedGpuPerformanceSample& operator=(const ScopedGpuPerformanceSample&) = delete;

private:
    ID3D11DeviceContext* context_{};
    GpuPerformanceQuerySlot* slot_{};
};

}  // namespace fo4hdr
