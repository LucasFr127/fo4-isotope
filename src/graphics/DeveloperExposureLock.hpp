// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fo4hdr {

// Render-thread policy only. A submitted capture becomes usable after an
// explicitly eligible close of that same frame; EndFrame alone proves nothing.
class DeveloperExposureLock final
{
public:
    using Frame = std::uint64_t;
    using GenerationId = std::uint64_t;

    constexpr void Request(bool requested) noexcept
    {
        if (!requested) Reset();
        else requested_ = true;
    }

    constexpr void Reset() noexcept
    {
        requested_ = false;
        active_ = false;
        capturedFrame_ = 0;
        observedFrame_ = 0;
        generation_ = 0;
    }

    // Record only after capture submission, pixel draw and restoration succeed.
    // Numeric validity stays on the GPU; active does not promise a valid pair.
    constexpr void Capture(Frame frame, GenerationId generation) noexcept
    {
        if (!NeedsCapture()) return;
        if (frame == 0) return;
        if (generation == 0) return;
        capturedFrame_ = frame;
        generation_ = generation;
    }

    constexpr void Observe(Frame frame, GenerationId generation) noexcept
    {
        if (!active_) return;
        if (frame == 0 || generation != generation_)
        {
            Reset();
            return;
        }
        observedFrame_ = frame;
    }

    constexpr void EndFrame(Frame frame, GenerationId generation, bool eligible) noexcept
    {
        if (!CompletionEligible(frame, generation, eligible))
        {
            Reset();
            return;
        }
        active_ = true;
        observedFrame_ = 0;
    }

    [[nodiscard]] constexpr bool Requested() const noexcept { return requested_; }
    [[nodiscard]] constexpr bool Active() const noexcept { return active_; }
    [[nodiscard]] constexpr Frame CapturedFrame() const noexcept { return capturedFrame_; }
    [[nodiscard]] constexpr GenerationId Generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr bool NeedsCapture() const noexcept
    {
        return requested_ && !active_ && capturedFrame_ == 0;
    }

private:
    [[nodiscard]] constexpr bool CompletionEligible(
        Frame frame, GenerationId generation, bool eligible) const noexcept
    {
        const auto completed = active_ ? observedFrame_ : capturedFrame_;
        return requested_ && eligible && frame != 0 && generation != 0 &&
            generation == generation_ && completed == frame;
    }

    bool requested_{};
    bool active_{};
    Frame capturedFrame_{};
    Frame observedFrame_{};
    GenerationId generation_{};
};

}  // namespace fo4hdr
