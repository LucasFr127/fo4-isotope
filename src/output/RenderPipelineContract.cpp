// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FO4Isotope/output/RenderPipelineContract.hpp"

#include "FO4Isotope/output/Hdr10Color.hpp"

#include <algorithm>
#include <cmath>

namespace fo4hdr {

namespace {

// HLSL saturate: NaN becomes zero.
[[nodiscard]] double Saturate(double value) noexcept
{
    return std::isnan(value) ? 0.0 : std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double TemporalPeak(double displayPeakNits) noexcept
{
    const auto peak = displayPeakNits / kSceneReferenceWhiteNits;
    return std::isnan(peak) ? 1.0 : std::max(peak, 1.0);
}

}  // namespace

double EncodeTemporalIntermediate(
    double sceneRelative, double displayPeakNits) noexcept
{
    return std::pow(Saturate(sceneRelative / TemporalPeak(displayPeakNits)), 1.0 / 2.2);
}

double DecodeTemporalIntermediate(
    double temporalCode, double displayPeakNits) noexcept
{
    return std::pow(Saturate(temporalCode), 2.2) * TemporalPeak(displayPeakNits);
}

double EncodeTemporalHistory(double temporalCode, double displayPeakNits) noexcept
{
    const auto peak = TemporalPeak(displayPeakNits);
    const auto light = std::pow(Saturate(temporalCode), 2.2);
    return std::pow(light * (peak + light) / (1.0 + light * peak), 1.0 / 2.2);
}

double DecodeTemporalHistory(double history, double displayPeakNits) noexcept
{
    const auto peak = TemporalPeak(displayPeakNits);
    const auto compressed = std::pow(Saturate(history), 2.2);
    const auto slope = peak * (1.0 - compressed);
    return std::pow(
        2.0 * compressed / (slope + std::sqrt(slope * slope + 4.0 * compressed)),
        1.0 / 2.2);
}

void PipelineHeartbeat::Reset(std::uint64_t generation) noexcept
{
    generation_ = generation;
    frame_ = 0;
    toneMapFamilyObserved_ = false;
    lutObserved_ = false;
    orderedPairObserved_ = false;
}

void PipelineHeartbeat::BeginFrame(
    std::uint64_t generation,
    std::uint64_t frame) noexcept
{
    Reset(generation);
    frame_ = frame;
}

void PipelineHeartbeat::Observe(RenoDxShaderPass pass) noexcept
{
    switch (pass)
    {
    case RenoDxShaderPass::toneMapA:
    case RenoDxShaderPass::toneMapB:
    case RenoDxShaderPass::toneMapNewA:
    case RenoDxShaderPass::toneMapNewB:
        // Fallout 4 can execute more than one image-space branch in a frame.
        // A later tone variant is another candidate branch, not proof that a
        // previously completed tone-to-LUT branch ran backwards.
        toneMapFamilyObserved_ = true;
        break;
    case RenoDxShaderPass::lut:
        lutObserved_ = true;
        orderedPairObserved_ |= toneMapFamilyObserved_;
        break;
    default:
        break;
    }
}

PipelineHeartbeatEvaluation PipelineHeartbeat::Evaluate(
    std::uint64_t generation,
    std::uint64_t frame) const noexcept
{
    const auto currentGeneration = generation != 0 && generation_ == generation;
    const auto currentFrame = frame != 0 && frame_ == frame;
    auto outcome = PipelineHeartbeatEvaluation::Outcome::incomplete;
    if (!currentGeneration)
        outcome = PipelineHeartbeatEvaluation::Outcome::staleGeneration;
    else if (!currentFrame)
        outcome = PipelineHeartbeatEvaluation::Outcome::staleFrame;
    else if (orderedPairObserved_)
        outcome = PipelineHeartbeatEvaluation::Outcome::ready;
    return {
        .currentGeneration = currentGeneration,
        .currentFrame = currentFrame,
        .toneMapFamilyObserved = toneMapFamilyObserved_,
        .lutObserved = lutObserved_,
        .ordered = orderedPairObserved_,
        .outcome = outcome,
        .ready = outcome == PipelineHeartbeatEvaluation::Outcome::ready
    };
}

}  // namespace fo4hdr
