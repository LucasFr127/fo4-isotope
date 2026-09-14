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

bool CurrentThreadMatches(const fo4hdr::NativeLutObservation& observation) noexcept
{
    return depth == 1 && observation.threadId == ::GetCurrentThreadId();
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
    observation_ = observation;
    observation_.serial = ++serial;
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
    if (resourceGeneration != generation.load(std::memory_order_acquire)) return false;
    result = *current;
    if (resourceGeneration != generation.load(std::memory_order_acquire))
    {
        result = {};
        return false;
    }
    return true;
}

void ResetNativeLutObservation(std::uint64_t resourceGeneration) noexcept
{
    generation.store(resourceGeneration, std::memory_order_release);
}

std::uint64_t NativeLutResourceGeneration() noexcept
{
    return generation.load(std::memory_order_acquire);
}

}  // namespace fo4hdr
