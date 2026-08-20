// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "Fallout4HDR/output/ArmorUiComposition.hpp"

#include <Windows.h>
#include <atomic>
#include <exception>
#include <limits>

namespace {
std::atomic<const fo4hdr::ArmorUiBoundary*> g_boundary{};
thread_local bool g_scopeActive{};
thread_local std::uint64_t g_serial{};
}

namespace fo4hdr {

void SetArmorUiBoundary(const ArmorUiBoundary* boundary) noexcept
{
    g_boundary.store(boundary, std::memory_order_release);
}

ArmorUiScope::ArmorUiScope(const ArmorUiScopeEntry& entry) noexcept
    : previousActive_(g_scopeActive), exceptionsOnEntry_(std::uncaught_exceptions())
{
    // Even rejected nested calls mask the outer scope and notify its owner.
    g_scopeActive = true;
    boundary_ = g_boundary.load(std::memory_order_acquire);
    token_.entry = entry;
    token_.nested = previousActive_;
    if (entry.threadId != ::GetCurrentThreadId()) token_.entry.owner = ArmorUiOwner::none;
    if (g_serial != std::numeric_limits<std::uint64_t>::max()) token_.serial = ++g_serial;
    if (boundary_ && boundary_->notify) boundary_->notify(boundary_->state, token_, true);
}

ArmorUiScope::~ArmorUiScope()
{
    token_.aborted = std::uncaught_exceptions() > exceptionsOnEntry_;
    if (boundary_ && boundary_->notify) boundary_->notify(boundary_->state, token_, false);
    g_scopeActive = previousActive_;
}

}  // namespace fo4hdr
