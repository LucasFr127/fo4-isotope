// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageSpaceObserver.hpp"

#include <cmath>
#include <cstring>

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

}  // namespace fo4hdr

namespace fo4hdr::game::imagespace_detail {

Publication::Token Publication::Begin() noexcept
{
    const auto active = active_.fetch_add(1, std::memory_order_acq_rel);
    return {revision_.fetch_add(1, std::memory_order_acq_rel) + 1, active == 0};
}

void Publication::Invalidate() noexcept
{
    revision_.fetch_add(1, std::memory_order_acq_rel);
}

bool Publication::Finish(Token& token, const Provenance& candidate) noexcept
{
    if (!token.revision) return false;
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
        if (active_.load(std::memory_order_acquire)) result.status = Status::updating;
        return false;
    }
    return true;
}

bool Publication::IsCurrent(std::uint64_t revision) const noexcept
{
    return revision != 0 && !active_.load(std::memory_order_acquire) &&
        revision_.load(std::memory_order_acquire) == revision;
}

bool Publication::CanPublish(const Token& token) const noexcept
{
    return token.exclusive &&
        active_.load(std::memory_order_acquire) == 1 &&
        revision_.load(std::memory_order_acquire) == token.revision;
}

}  // namespace fo4hdr::game::imagespace_detail
