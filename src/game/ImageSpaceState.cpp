// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageSpaceObserver.hpp"

namespace fo4hdr::game::imagespace_detail {

std::uint64_t Publication::AdvanceRevision() noexcept
{
    auto previous = revision_.load(std::memory_order_relaxed);
    if (previous == UINT64_MAX) return 0;
    if (revision_.compare_exchange_strong(previous, previous + 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) return previous + 1;
    // Collision retires the publication instead of trusting the other writer.
    revision_.store(UINT64_MAX, std::memory_order_release);
    return 0;
}

Publication::Token Publication::Begin() noexcept
{
    const auto active = active_.fetch_add(1, std::memory_order_acq_rel);
    return {AdvanceRevision(), active == 0, true};
}

void Publication::Invalidate() noexcept
{
    (void)AdvanceRevision();
}

bool Publication::Finish(Token& token, const Provenance& candidate) noexcept
{
    if (!token.entered) return false;
    bool published{};
    {
        TryPublicationGuard guard(flag_);
        if (guard.OwnsLock() && CanPublish(token))
        {
            payload_ = candidate;
            payload_.revision = token.revision;
            published = true;
        }
    }
    // No guard is held across native work; another writer revokes this copy.
    token = {};
    active_.fetch_sub(1, std::memory_order_acq_rel);
    return published;
}

bool Publication::TryRead(Provenance& result) noexcept
{
    result = {};
    if (active_.load(std::memory_order_acquire))
    {
        result.status = Status::updating;
        return false;
    }
    const auto revision = revision_.load(std::memory_order_acquire);
    TryPublicationGuard guard(flag_);
    if (!guard.OwnsLock())
    {
        result.status = Status::updating;
        return false;
    }
    result = payload_;
    if (!IsCurrent(revision) || result.revision != revision)
    {
        result = {};
        result.status = active_.load(std::memory_order_acquire) ? Status::updating : Status::stale;
        return false;
    }
    return true;
}

bool Publication::IsCurrent(std::uint64_t revision) const noexcept
{
    return revision != 0 && revision != UINT64_MAX && !active_.load(std::memory_order_acquire) &&
        revision_.load(std::memory_order_acquire) == revision;
}

bool Publication::CanPublish(const Token& token) const noexcept
{
    return token.revision != 0 && token.revision != UINT64_MAX && token.exclusive &&
        active_.load(std::memory_order_acquire) == 1 &&
        revision_.load(std::memory_order_acquire) == token.revision;
}

}  // namespace fo4hdr::game::imagespace_detail
