// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace fo4hdr {

class SharedUiResourceReadiness
{
    static constexpr unsigned kReady = 1, kDisabled = 2;
    std::atomic<unsigned> state_{};
public:
    void SetReady(bool ready) noexcept
    {
        if (ready) state_.fetch_or(kReady, std::memory_order_release);
        else state_.fetch_and(~kReady, std::memory_order_release);
    }
    void Disable() noexcept
    {
        // One atomic state prevents an overlapping completion from reviving a faulted pair.
        state_.fetch_or(kDisabled, std::memory_order_release);
    }
    [[nodiscard]] bool Ready() const noexcept
    {
        return state_.load(std::memory_order_acquire) == kReady;
    }
};

struct SharedUiFrameIdentity
{
    std::uint64_t generation{};
    std::uint64_t context{};
    std::uint64_t frame{};
    std::uint64_t request{};
    bool operator==(const SharedUiFrameIdentity&) const = default;

    [[nodiscard]] bool Valid() const noexcept
    {
        return generation != 0 && context != 0 && frame != 0;
    }
};

struct SharedUiContent
{
    SharedUiFrameIdentity identity{};
    std::uint64_t revision{};
    std::uint64_t scope{};
    float white{};
    bool seeded{};
    bool closed{};
    bool rejected{};
    bool nativeWritten{};

    [[nodiscard]] bool Begin(SharedUiFrameIdentity next, std::uint64_t serial,
        float paperWhite) noexcept
    {
        if (seeded || rejected) return false;
        if (!next.Valid() || serial == 0) return false;
        if (!std::isfinite(paperWhite) || paperWhite <= 0.0F) return false;
        identity = next;
        scope = serial;
        white = paperWhite;
        ++revision;
        seeded = true;
        closed = false;
        nativeWritten = false;
        return true;
    }

    void Close(SharedUiFrameIdentity current, std::uint64_t serial) noexcept
    {
        if (!seeded) return;
        const bool matches = identity == current && scope == serial;
        if (!matches || closed) rejected = true;
        closed = true;
    }

    [[nodiscard]] bool Open(SharedUiFrameIdentity current, std::uint64_t serial) noexcept
    {
        if (!seeded || rejected) return false;
        if (!closed || identity != current) return false;
        if (serial == 0 || serial == scope) return false;
        scope = serial;
        closed = false;
        return true;
    }

    [[nodiscard]] bool ScopeActive(SharedUiFrameIdentity current, std::uint64_t serial) const noexcept
    {
        return seeded && !closed && !rejected && identity == current && scope == serial;
    }

    void ObserveNativeWrite(std::uint64_t serial) noexcept
    {
        if (!seeded || closed) return;
        if (scope == serial) nativeWritten = true;
    }

    [[nodiscard]] bool Ready(SharedUiFrameIdentity current) const noexcept
    {
        return seeded && closed && !rejected && identity == current;
    }

    void Advance() noexcept
    {
        // Fresh-frame qualification is separate from the public image's readable interpretation.
        seeded = false;
        closed = false;
        rejected = false;
        nativeWritten = false;
    }
};

} // namespace fo4hdr
