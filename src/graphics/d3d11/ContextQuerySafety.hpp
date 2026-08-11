// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>

namespace fo4hdr {

// Opaque keys remain alive for the caller's complete Begin/End interval.
// Default state assumes observation from an owned context's first operation;
// unobserved coverage is sticky until this record is retired.
struct ContextQuerySafety
{
    static constexpr std::size_t capacity = 32;
    std::array<const void*, capacity> active{};
    bool unknown{};
    std::size_t executionDepth{};

    void Begin(const void* key) noexcept;
    void End(const void* key) noexcept;
    [[nodiscard]] bool EnterCommandList() noexcept;
    void LeaveCommandList() noexcept;
    [[nodiscard]] bool CanDispatch() const noexcept;
};

}  // namespace fo4hdr
