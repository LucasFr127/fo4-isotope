// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "passes/PresentationPass.hpp"
#include "Fallout4HDR/output/PerformanceStatistics.hpp"
#include "TestHarness.hpp"

#include <profileapi.h>
#include <stdexcept>

namespace fo4hdr::tests::gpu::detail {

inline void CheckWarmingSubmission(double milliseconds)
{
    FixedPerformanceStatistics statistics;
    FO4HDR_CHECK(statistics.AddSample(milliseconds) == PerformanceSampleResult::warmingUp);
    FO4HDR_CHECK(statistics.WarmupSamples() == 1);
    FO4HDR_CHECK(statistics.MeasuredSamples() == 0);
    FO4HDR_CHECK(!statistics.Complete());
}

inline LONGLONG SubmissionCounterFrequency()
{
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency))
        throw std::runtime_error("CPU performance frequency unavailable");
    if (frequency.QuadPart <= 0) throw std::runtime_error("Invalid CPU performance frequency");
    return frequency.QuadPart;
}

inline double TerminalSubmissionMilliseconds(PresentationPass& pass, ID3D11UnorderedAccessView* terminal)
{
    const auto frequency = SubmissionCounterFrequency();
    LARGE_INTEGER begin{}, end{};
    const auto began = QueryPerformanceCounter(&begin);
    const auto executed = pass.ExecuteCurrentScene(terminal, PresentationScope::qualifiedExclusive);
    const auto ended = QueryPerformanceCounter(&end);
    if (!began || !ended) throw std::runtime_error("CPU performance interval unavailable");
    if (end.QuadPart < begin.QuadPart) throw std::runtime_error("CPU performance counter moved backwards");
    if (!executed) throw std::runtime_error("Timed terminal submission failed");
    return static_cast<double>(end.QuadPart - begin.QuadPart) *
        1000.0 / static_cast<double>(frequency);
}

inline void MeasureEmptyLayerSubmission(PresentationPass& pass, ID3D11UnorderedAccessView* terminal)
{
    if (!pass.PrepareLayers(nullptr, nullptr, PresentationScope::qualifiedExclusive))
        throw std::runtime_error("Empty layer preparation failed");
    CheckWarmingSubmission(TerminalSubmissionMilliseconds(pass, terminal));
}

inline bool SubmitPreparedLayers(PresentationPass& pass, ID3D11UnorderedAccessView* terminal,
    bool measureSubmission)
{
    if (!measureSubmission) return pass.ExecuteCurrentScene(terminal, PresentationScope::qualifiedExclusive);
    CheckWarmingSubmission(TerminalSubmissionMilliseconds(pass, terminal));
    return true;
}

}  // namespace fo4hdr::tests::gpu::detail
