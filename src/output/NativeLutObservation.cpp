// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <cmath>
#include <limits>

namespace {
thread_local const fo4hdr::NativeLutObservation* current{};
thread_local std::uint64_t serial{};
thread_local std::uint32_t depth{};
std::atomic<std::uint64_t> generation{};
std::atomic<std::uint64_t> revocation{1}, imageSpaceRevision{1};

std::uint64_t AdvanceRevision(std::atomic<std::uint64_t>& revision) noexcept
{
    auto previous = revision.load(std::memory_order_relaxed);
    if (previous == UINT64_MAX) return 0;
    if (revision.compare_exchange_strong(previous, previous + 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) return previous + 1;
    // A competing publication cannot satisfy a failed revocation.
    revision.store(UINT64_MAX, std::memory_order_release);
    return 0;
}

bool CurrentThreadMatches(const fo4hdr::NativeLutObservation& observation) noexcept
{
    return depth == 1 && observation.threadId == ::GetCurrentThreadId();
}

bool CurrentSerialMatches(const fo4hdr::NativeLutObservation& observation,
    std::uint32_t threadId) noexcept
{
    if (current == nullptr || !CurrentThreadMatches(*current)) return false;
    if (threadId == 0 || threadId != ::GetCurrentThreadId()) return false;
    return observation.threadId == threadId && observation.serial != 0 &&
        observation.serial == current->serial;
}

bool CurrentImageSpaceRevisionMatches(std::uint64_t revision) noexcept
{
    if (revision == 0 || revision == UINT64_MAX) return false;
    return revision == imageSpaceRevision.load(std::memory_order_acquire);
}
}

namespace fo4hdr {

bool DecodeImageSpaceActiveData(std::span<const std::byte> source,
    const std::array<std::size_t, kImageSpaceActiveDataFieldCount>& offsets,
    ImageSpaceActiveData& destination) noexcept
{
    std::array<float, kImageSpaceActiveDataFieldCount> values{};
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto offset = offsets[index];
        if (offset > source.size() || sizeof(float) > source.size() - offset) return false;
        std::memcpy(&values[index], source.data() + offset, sizeof(float));
        if (!std::isfinite(values[index])) return false;
    }
    destination = {values[0], values[1], values[2], values[3], values[4], values[5],
        values[6], values[7], values[8], values[9], values[10], values[11]};
    return true;
}

NativeLutScope::NativeLutScope(const NativeLutObservation& observation) noexcept :
    previous_(current)
{
    current = nullptr;
    ++depth;
    if (!CurrentThreadMatches(observation)) return;
    if (observation.validSlots == 0 || serial == UINT64_MAX) return;
    const auto epoch = revocation.load(std::memory_order_acquire);
    if (observation.generation != 0 && observation.generation != epoch) return;
    observation_ = observation;
    observation_.serial = ++serial;
    observation_.generation = epoch;
    current = &observation_;
}

NativeLutScope::~NativeLutScope()
{
    current = previous_;
    --depth;
}

bool ReadCurrentNativeLutObservation(
    std::uint64_t resourceGeneration, NativeLutObservation& result) noexcept
{
    result = {};
    if (current == nullptr || !CurrentThreadMatches(*current)) return false;
    if (current->resourceGeneration != resourceGeneration) return false;
    if (!IsCurrentNativeLutObservation(*current, ::GetCurrentThreadId())) return false;
    if (resourceGeneration != generation.load(std::memory_order_acquire)) return false;
    result = *current;
    if (!IsCurrentNativeLutObservation(result, ::GetCurrentThreadId()))
    {
        result = {};
        return false;
    }
    return true;
}

void ResetNativeLutObservation(std::uint64_t resourceGeneration) noexcept
{
    RevokeNativeLutObservation();
    generation.store(resourceGeneration, std::memory_order_release);
}

std::uint64_t NativeLutResourceGeneration() noexcept
{
    return generation.load(std::memory_order_acquire);
}

bool IsCurrentNativeLutObservation(
    const NativeLutObservation& observation, std::uint32_t threadId) noexcept
{
    if (!CurrentSerialMatches(observation, threadId)) return false;
    const auto epoch = revocation.load(std::memory_order_acquire);
    if (epoch == UINT64_MAX || observation.generation != epoch) return false;
    return observation.resourceGeneration == generation.load(std::memory_order_acquire);
}

std::uint32_t EligibleImageSpaceSlots(
    const NativeLutObservation& observation, std::uint32_t validatedLuts) noexcept
{
    if (!IsCurrentNativeLutObservation(observation, ::GetCurrentThreadId())) return 0;
    if (observation.imageSpaceStatus != ImageSpaceObservationStatus::matched) return 0;
    if (!CurrentImageSpaceRevisionMatches(observation.imageSpaceRevision)) return 0;
    return observation.imageSpaceValidSlots & observation.validSlots & validatedLuts & 15U;
}

void RevokeNativeLutObservation() noexcept
{
    (void)AdvanceRevision(revocation);
    (void)InvalidateImageSpaceObservation();
}

std::uint64_t NativeLutObservationGeneration() noexcept
{
    return revocation.load(std::memory_order_acquire);
}

std::uint64_t CurrentImageSpaceObservationRevision() noexcept
{
    return imageSpaceRevision.load(std::memory_order_acquire);
}

std::uint64_t InvalidateImageSpaceObservation() noexcept
{
    return AdvanceRevision(imageSpaceRevision);
}

}  // namespace fo4hdr
