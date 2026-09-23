// SPDX-License-Identifier: GPL-3.0-or-later

#include "ContextQuerySafety.hpp"

namespace fo4hdr {

bool ContextQuerySafety::EnterCommandList() noexcept
{
    if (executionDepth == capacity) { unknown = true; return false; }
    ++executionDepth;
    return true;
}

void ContextQuerySafety::LeaveCommandList() noexcept
{
    if (executionDepth == 0) { unknown = true; return; }
    --executionDepth;
}

void ContextQuerySafety::Begin(const void* key) noexcept
{
    if (key == nullptr) { unknown = true; return; }
    if (std::find(active.begin(), active.end(), key) != active.end())
    {
        unknown = true;
        return;
    }
    const auto empty = std::find(active.begin(), active.end(), nullptr);
    if (empty == active.end()) { unknown = true; return; }
    *empty = key;
}

void ContextQuerySafety::End(const void* key) noexcept
{
    if (key == nullptr) return;
    for (auto& entry : active)
        if (entry == key) { entry = nullptr; return; }
    // End-only timestamp/event queries do not clear unrelated scopes.
}

bool ContextQuerySafety::CanDispatch() const noexcept
{
    if (unknown || executionDepth != 0) return false;
    for (const auto* entry : active)
        if (entry != nullptr) return false;
    return true;
}

} // namespace fo4hdr
