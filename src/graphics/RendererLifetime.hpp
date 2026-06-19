// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace fo4hdr {

class RendererLifetime
{
public:
    static constexpr auto terminalEpoch = (std::numeric_limits<std::uint64_t>::max)() - 1;

    // Exclusive setup supplies a nonzero even start and an exclusive even bound once.
    [[nodiscard]] bool Prepare(std::uint64_t initialEpoch, std::uint64_t epochLimit) noexcept;
    // After preparation, call once after complete installation before callbacks or retirement.
    void PublishInitial() noexcept;
    [[nodiscard]] std::uint64_t CurrentEpoch() const noexcept;
    [[nodiscard]] std::uint64_t BeginResize(std::uint64_t epoch) noexcept;
    // Complete only a BeginResize transition after native success and ownership qualification.
    void CompleteResize(std::uint64_t transition) noexcept;
    void Retire() noexcept;

private:
    std::uint64_t epochLimit_{};
    std::atomic<std::uint64_t> epoch_{};
};

}  // namespace fo4hdr
