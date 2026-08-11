// SPDX-License-Identifier: GPL-3.0-or-later

#include "ContextQuerySafety.hpp"

#include <algorithm>

namespace fo4hdr {

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
    const auto found = std::find(active.begin(), active.end(), key);
    if (key == nullptr || found == active.end()) { unknown = true; return; }
    *found = nullptr;
}

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

bool ContextQuerySafety::CanDispatch() const noexcept
{
    if (unknown || executionDepth != 0) return false;
    return std::all_of(active.begin(), active.end(), [](const void* key) {
        return key == nullptr;
    });
}

}  // namespace fo4hdr
