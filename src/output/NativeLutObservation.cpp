// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/output/NativeLutObservation.hpp"

#include <atomic>

namespace fo4hdr {
namespace {
std::atomic<NativeLutObserverStatus> g_status{};
std::atomic<bool> g_imageSpaceReady{};
std::atomic<std::uint64_t> g_imageSpaceRevision{1};
std::atomic<std::uint64_t> g_generation{1};
thread_local NativeLutObservation g_current{};
thread_local std::uint64_t g_serial{};
thread_local std::uint32_t g_depth{};
}

void SetNativeLutObserverStatus(NativeLutObserverStatus status) noexcept
{
    g_status.store(status, std::memory_order_release);
    if (status != NativeLutObserverStatus::ready) ResetNativeLutObservation();
}

NativeLutObserverStatus GetNativeLutObserverStatus() noexcept
{
    return g_status.load(std::memory_order_acquire);
}

void ResetNativeLutObservation() noexcept
{
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t NativeLutObservationGeneration() noexcept
{
    return g_generation.load(std::memory_order_acquire);
}

void SetImageSpaceObservationReady(bool ready) noexcept
{
    g_imageSpaceReady.store(ready, std::memory_order_release);
}

bool IsImageSpaceObservationReady() noexcept
{
    return g_imageSpaceReady.load(std::memory_order_acquire);
}

std::uint64_t CurrentImageSpaceObservationRevision() noexcept
{
    return g_imageSpaceRevision.load(std::memory_order_acquire);
}

std::uint64_t InvalidateImageSpaceObservation() noexcept
{
    return g_imageSpaceRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint32_t EligibleImageSpaceSlots(
    const NativeLutObservation& observation, std::uint32_t validatedLuts) noexcept
{
    return IsImageSpaceObservationReady() &&
        observation.imageSpaceStatus == ImageSpaceObservationStatus::matched &&
        observation.imageSpaceRevision != 0 &&
        observation.imageSpaceRevision == CurrentImageSpaceObservationRevision() &&
        observation.generation == NativeLutObservationGeneration() ?
        observation.imageSpaceValidSlots & validatedLuts & 15U : 0U;
}

const NativeLutObservation& CurrentNativeLutObservation() noexcept
{
    return g_current;
}

bool IsCurrentNativeLutObservation(
    const NativeLutObservation& observation, std::uint32_t threadId) noexcept
{
    return GetNativeLutObserverStatus() == NativeLutObserverStatus::ready &&
        observation.serial != 0 && observation.serial == g_current.serial &&
        observation.threadId == threadId && threadId != 0 &&
        observation.generation == g_generation.load(std::memory_order_acquire);
}

NativeLutScope::NativeLutScope(const NativeLutObservation& observation) noexcept :
    previous_(g_current)
{
    g_current = {};
    ++g_depth;
    if (g_depth > 8 || GetNativeLutObserverStatus() != NativeLutObserverStatus::ready)
        return;
    g_current = observation;
    if (++g_serial == 0) ++g_serial;
    g_current.serial = g_serial;
    g_current.generation = g_generation.load(std::memory_order_acquire);
}

NativeLutScope::~NativeLutScope()
{
    g_current = previous_.generation == g_generation.load(std::memory_order_acquire) ?
        previous_ : NativeLutObservation{};
    --g_depth;
}

}  // namespace fo4hdr
