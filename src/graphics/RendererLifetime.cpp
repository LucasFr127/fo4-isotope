// SPDX-License-Identifier: GPL-3.0-or-later

#include "RendererLifetime.hpp"

namespace fo4hdr {

bool RendererLifetime::Prepare(std::uint64_t initialEpoch, std::uint64_t epochLimit) noexcept
{
    if (epoch_.load(std::memory_order_relaxed) != 0 || initialEpoch == 0)
    {
        return false;
    }
    if ((initialEpoch & 1) != 0 || (epochLimit & 1) != 0)
    {
        return false;
    }
    if (initialEpoch >= epochLimit || epochLimit > terminalEpoch)
    {
        return false;
    }
    epochLimit_ = epochLimit;
    epoch_.store(initialEpoch, std::memory_order_relaxed);
    return true;
}

void RendererLifetime::PublishInitial() noexcept
{
    epoch_.fetch_add(1, std::memory_order_release);
}

std::uint64_t RendererLifetime::CurrentEpoch() const noexcept
{
    return epoch_.load(std::memory_order_acquire);
}

std::uint64_t RendererLifetime::BeginResize(std::uint64_t epoch) noexcept
{
    if (epochLimit_ == 0)
    {
        return terminalEpoch;
    }
    if (epoch >= epochLimit_ - 2)
    {
        static_cast<void>(epoch_.compare_exchange_strong(epoch, terminalEpoch,
            std::memory_order_acq_rel));
        return terminalEpoch;
    }
    const auto transition = (epoch | 1) + 1;
    if (!epoch_.compare_exchange_strong(epoch, transition, std::memory_order_acq_rel))
    {
        return terminalEpoch;
    }
    return transition;
}

void RendererLifetime::CompleteResize(std::uint64_t transition) noexcept
{
    if (transition == terminalEpoch)
    {
        return;
    }
    static_cast<void>(epoch_.compare_exchange_strong(transition, transition + 1,
        std::memory_order_acq_rel));
}

void RendererLifetime::Retire() noexcept
{
    epoch_.store(terminalEpoch, std::memory_order_release);
}

}  // namespace fo4hdr
