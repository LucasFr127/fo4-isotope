// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "Fallout4HDR/output/RenderPipelineContract.hpp"

#include <algorithm>
#include <cmath>

namespace fo4hdr {

namespace {

constexpr double kMaximumFiniteFp16 = 65504.0;
constexpr double kMaximumTemporalCode =
    kMaximumFiniteFp16 / (1.0 + kMaximumFiniteFp16);

}  // namespace

double EncodeTemporalIntermediate(double linear) noexcept
{
    if (std::isnan(linear) || linear <= 0.0) return 0.0;
    if (!std::isfinite(linear)) return std::pow(kMaximumTemporalCode, 1.0 / 2.2);
    const auto positive = std::min(linear, kMaximumFiniteFp16);
    return std::pow(positive / (1.0 + positive), 1.0 / 2.2);
}

double DecodeTemporalIntermediate(double encoded) noexcept
{
    if (std::isnan(encoded) || encoded <= 0.0) return 0.0;
    if (!std::isfinite(encoded)) return kMaximumFiniteFp16;
    const auto compressed = std::min(
        std::pow(std::clamp(encoded, 0.0, 1.0), 2.2),
        kMaximumTemporalCode);
    return std::min(
        compressed / std::max(1.0 - compressed, 1.0 / 65505.0),
        kMaximumFiniteFp16);
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
