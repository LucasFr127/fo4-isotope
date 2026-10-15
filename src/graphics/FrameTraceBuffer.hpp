// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/hooks/HookOwnership.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace fo4hdr {

enum class FrameTraceStop
{
    idle,
    recording,
    complete,
    capacity,
    retired
};

template <class Event, std::size_t Capacity>
class FrameTraceBuffer
{
    static_assert(std::is_trivially_copyable_v<Event>);
    static_assert(Capacity > 0);

public:
    // Parenthesized value-initialization keeps large trace arrays compact for MSVC.
    FrameTraceBuffer() noexcept(std::is_nothrow_default_constructible_v<Event>) :
        events_()
    {}

    [[nodiscard]] bool HotkeyPressed(bool down) noexcept
    {
        // Present may migrate to the loading thread while the chord remains held.
        const bool previous = hotkeyDown_.exchange(down, std::memory_order_relaxed);
        return down && !previous;
    }

    [[nodiscard]] bool Begin() noexcept
    {
        return Begin([]() noexcept {});
    }

    template <class Start>
    [[nodiscard]] bool Begin(Start&& start, bool rolling = false) noexcept
    {
        static_assert(std::is_nothrow_invocable_v<Start>);
        BoundedAtomicFlagGuard guard(lock_, 1);
        if (!guard.OwnsLock()) return false;
        if (State() != FrameTraceStop::idle || participants_.load() != 0)
            return false;
        count_ = 0;
        next_ = 0;
        sequence_ = 0;
        rolling_ = rolling;
        dropped_.store(0, std::memory_order_relaxed);
        overwritten_.store(0, std::memory_order_relaxed);
        epoch_.fetch_add(1);
        std::forward<Start>(start)();
        state_.store(FrameTraceStop::recording);
        return true;
    }

    [[nodiscard]] bool Recording() const noexcept
    {
        return State() == FrameTraceStop::recording;
    }

    [[nodiscard]] FrameTraceStop State() const noexcept
    {
        return state_.load();
    }

    [[nodiscard]] std::uint64_t Epoch() const noexcept { return epoch_.load(); }
    [[nodiscard]] std::uint64_t Overwritten() const noexcept { return overwritten_.load(); }

    template <class Observer>
    [[nodiscard]] bool WithCapture(std::uint64_t epoch, Observer&& observer) noexcept
    {
        static_assert(std::is_nothrow_invocable_v<Observer>);
        RecordScope participant(participants_);
        if (!Recording() || epoch != Epoch()) return false;
        std::forward<Observer>(observer)();
        return true;
    }

    template <class Writer>
    [[nodiscard]] bool Record(Writer&& writer) noexcept
    {
        return Record(Epoch(), std::forward<Writer>(writer));
    }

    template <class Writer>
    [[nodiscard]] bool Record(std::uint64_t epoch, Writer&& writer) noexcept
    {
        static_assert(std::is_nothrow_invocable_v<Writer, Event&, std::uint64_t>);
        // Register before reading state so reporting cannot retire a pending contender's capture.
        RecordScope participant(participants_);
        if (!Recording()) return false;
        BoundedAtomicFlagGuard guard(lock_, 1);
        if (!guard.OwnsLock())
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (epoch != epoch_.load())
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!Recording())
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::forward<Writer>(writer)(events_[next_], ++sequence_);
        AdvanceCursor();
        return true;
    }

    void Stop(FrameTraceStop reason) noexcept
    {
        if (!Frozen(reason)) return;
        auto expected = FrameTraceStop::recording;
        state_.compare_exchange_strong(expected, reason);
    }

    template <class Reader>
    [[nodiscard]] bool Report(Reader&& reader, std::uint64_t expectedEpoch = 0)
    {
        if (!Frozen(State())) return false;
        // Stop can freeze an in-flight writer; the guard publishes its completed event.
        BoundedAtomicFlagGuard guard(lock_, 1);
        if (!guard.OwnsLock()) return false;
        const auto reason = State();
        if (!CanReport(reason, expectedEpoch)) return false;
        // Normalize before invoking a throwable sink so retry observes the same chronological tail.
        if (count_ == Capacity && next_ != 0)
            std::rotate(events_.begin(), events_.begin() + next_, events_.end());
        next_ = 0;
        std::forward<Reader>(reader)(std::span<const Event>(events_.data(), count_), reason,
            dropped_.load(std::memory_order_relaxed));
        state_.store(FrameTraceStop::idle);
        return true;
    }

private:
    [[nodiscard]] bool CanReport(FrameTraceStop state, std::uint64_t expectedEpoch) const noexcept
    {
        return Frozen(state) && participants_.load() == 0 &&
            (expectedEpoch == 0 || expectedEpoch == Epoch());
    }

    void AdvanceCursor() noexcept
    {
        if (count_ == Capacity) overwritten_.fetch_add(1, std::memory_order_relaxed);
        else ++count_;
        next_ = (next_ + 1) % Capacity;
        if (count_ == Capacity && !rolling_) Stop(FrameTraceStop::capacity);
    }

    class RecordScope
    {
    public:
        explicit RecordScope(std::atomic<std::size_t>& participants) noexcept :
            participants_(participants)
        {
            participants_.fetch_add(1);
        }

        ~RecordScope() { participants_.fetch_sub(1); }

        RecordScope(const RecordScope&) = delete;
        RecordScope& operator=(const RecordScope&) = delete;

    private:
        std::atomic<std::size_t>& participants_;
    };

    [[nodiscard]] static bool Frozen(FrameTraceStop state) noexcept
    {
        return state == FrameTraceStop::complete || state == FrameTraceStop::capacity ||
            state == FrameTraceStop::retired;
    }

    std::array<Event, Capacity> events_;
    std::size_t count_{}, next_{};
    std::uint64_t sequence_{};
    bool rolling_{};
    std::atomic<std::uint64_t> dropped_{};
    std::atomic<std::uint64_t> overwritten_{};
    std::atomic<std::uint64_t> epoch_{};
    std::atomic<std::size_t> participants_{};
    std::atomic<FrameTraceStop> state_{FrameTraceStop::idle};
    std::atomic<bool> hotkeyDown_{};
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

} // namespace fo4hdr
