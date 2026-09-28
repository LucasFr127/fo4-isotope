// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <cstdint>

namespace fo4hdr {

// Render-thread policy for a developer exposure lock. A capture made during a
// frame becomes active only when that same frame reaches its eligible end.
class DeveloperExposureLock final
{
public:
    using Frame = std::uint64_t;
    using GenerationId = std::uint64_t;

    constexpr void Request(bool requested) noexcept
    {
        requested_ = requested;
        if (!requested_ && !active_)
        {
            capturedFrame_ = 0;
            generation_ = 0;
        }
    }

    // Hard lifecycle retirement. The user must explicitly request a new lock.
    constexpr void Reset() noexcept
    {
        requested_ = false;
        active_ = false;
        capturedFrame_ = 0;
        generation_ = 0;
    }

    // This records only that the caller completed its GPU capture; validating
    // the captured numeric value remains the caller's responsibility. Frame
    // and generation identifiers use zero as the unbound sentinel.
    constexpr void Capture(Frame frame, GenerationId generation) noexcept
    {
        if (!NeedsCapture()) return;
        if (frame == 0 || generation == 0) return;
        capturedFrame_ = frame;
        generation_ = generation;
    }

    constexpr void EndFrame(
        Frame frame, GenerationId generation, bool eligible) noexcept
    {
        const bool generationChanged = generation_ != 0 && generation_ != generation;
        if (!eligible || generationChanged)
        {
            Reset();
            return;
        }

        if (!requested_)
        {
            Reset();
            return;
        }

        if (active_) return;

        if (capturedFrame_ == 0) return;
        if (capturedFrame_ != frame)
        {
            Reset();
            return;
        }

        active_ = true;
    }

    [[nodiscard]] constexpr bool Requested() const noexcept
    {
        return requested_;
    }

    [[nodiscard]] constexpr bool Active() const noexcept
    {
        return active_;
    }

    [[nodiscard]] constexpr Frame CapturedFrame() const noexcept
    {
        return capturedFrame_;
    }

    [[nodiscard]] constexpr GenerationId Generation() const noexcept
    {
        return generation_;
    }

    [[nodiscard]] constexpr bool NeedsCapture() const noexcept
    {
        return requested_ && !active_ && capturedFrame_ == 0;
    }

private:
    bool requested_{};
    bool active_{};
    Frame capturedFrame_{};
    GenerationId generation_{};
};

}  // namespace fo4hdr
