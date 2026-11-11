// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fo4hdr {

// Counts the color grading draws where the automatic weather response stopped
// applying, by the reason given for each. The render thread records; the
// counts are atomic so a report on another thread can read them.
class WeatherResponseDrops final
{
public:
    using Frame = std::uint64_t;
    static constexpr std::size_t kCapacity = 16;
    // A response that returns within this many frames reads as a blink, not a scene change.
    static constexpr Frame kBriefFrames = 30;

    struct Entry
    {
        const char* reason{};
        std::uint32_t drops{};
        std::uint32_t brief{};
        std::uint32_t longestBriefFrames{};
    };

    // A null reason means the response applied to this draw. Reasons are
    // compared by address, so each must be a string with static lifetime.
    void Observe(Frame frame, const char* reason) noexcept
    {
        if (reason == nullptr)
        {
            Recover(frame);
            return;
        }
        // Only a draw that follows an applied one starts a drop.
        if (!applied_) return;
        applied_ = false;
        dropFrame_ = frame;
        open_ = Find(reason);
        if (open_ != nullptr)
            open_->drops.fetch_add(1, std::memory_order_relaxed);
        else
            unlisted_.fetch_add(1, std::memory_order_relaxed);
    }

    // The response was turned off, so its absence is not a drop.
    void Forget() noexcept
    {
        applied_ = false;
        open_ = nullptr;
    }

    [[nodiscard]] std::uint32_t Unlisted() const noexcept
    {
        return unlisted_.load(std::memory_order_relaxed);
    }

    template <class Visit>
    void ForEach(Visit visit) const
    {
        for (const auto& slot : slots_)
        {
            const auto* reason = slot.reason.load(std::memory_order_acquire);
            if (reason == nullptr) return;
            visit(Entry{reason, slot.drops.load(std::memory_order_relaxed),
                slot.brief.load(std::memory_order_relaxed),
                slot.longestBriefFrames.load(std::memory_order_relaxed)});
        }
    }

private:
    struct Slot
    {
        std::atomic<const char*> reason{};
        std::atomic<std::uint32_t> drops{};
        std::atomic<std::uint32_t> brief{};
        std::atomic<std::uint32_t> longestBriefFrames{};
    };

    void Recover(Frame frame) noexcept
    {
        applied_ = true;
        auto* slot = open_;
        open_ = nullptr;
        if (slot == nullptr) return;
        // An earlier frame wraps to a large gap and is not counted.
        const Frame gap = frame - dropFrame_;
        if (gap > kBriefFrames) return;
        slot->brief.fetch_add(1, std::memory_order_relaxed);
        slot->longestBriefFrames.store(
            std::max(static_cast<std::uint32_t>(gap),
                slot->longestBriefFrames.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
    }

    // Slots fill in order and are never released; a full table returns null.
    [[nodiscard]] Slot* Find(const char* reason) noexcept
    {
        for (auto& slot : slots_)
        {
            const auto* known = slot.reason.load(std::memory_order_relaxed);
            if (known == nullptr) slot.reason.store(reason, std::memory_order_release);
            if (known == nullptr || known == reason) return &slot;
        }
        return nullptr;
    }

    std::array<Slot, kCapacity> slots_{};
    std::atomic<std::uint32_t> unlisted_{};
    Slot* open_{};
    Frame dropFrame_{};
    bool applied_{};
};

}  // namespace fo4hdr
