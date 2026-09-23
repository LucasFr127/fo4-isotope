// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/RenderPipelineContract.hpp"

#include <algorithm>
#include <cmath>

namespace fo4hdr {

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
    if (generation_ != generation) Reset(generation);
    frame_ = frame;
    toneMapFamilyObserved_ = false;
    lutObserved_ = false;
    orderedPairObserved_ = false;
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
