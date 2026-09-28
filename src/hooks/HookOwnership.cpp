// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/hooks/HookOwnership.hpp"

namespace fo4hdr {
namespace {

[[nodiscard]] bool IsUsableHookTarget(void* next, void* replacement) noexcept
{
    return next != nullptr && replacement != nullptr && next != replacement;
}

}  // namespace

bool PointerHookOwnership::Install(
    void** slot,
    void* replacement,
    HookPointerWriter writer,
    HookNextPublisher publishNext) noexcept
{
    auto expectedState = HookOwnershipState::empty;
    if (!state_.compare_exchange_strong(
            expectedState,
            HookOwnershipState::installing,
            std::memory_order_acq_rel))
    {
        return false;
    }
    const auto observed = AtomicPointerLoad(slot);
    if (!IsUsableHookTarget(observed, replacement) || writer == nullptr)
    {
        state_.store(HookOwnershipState::installFailed, std::memory_order_release);
        return false;
    }
    slot_ = slot;
    next_ = observed;
    replacement_ = replacement;
    if (publishNext != nullptr && !publishNext(next_))
    {
        state_.store(HookOwnershipState::installFailed, std::memory_order_release);
        return false;
    }
    if (!writer(slot_, next_, replacement_))
    {
        state_ = HookOwnershipState::installFailed;
        return false;
    }
    state_.store(HookOwnershipState::owned, std::memory_order_release);
    return true;
}

bool PointerHookOwnership::Remove(HookPointerWriter writer) noexcept
{
    if (state_.load(std::memory_order_acquire) != HookOwnershipState::owned || writer == nullptr)
    {
        return false;
    }
    if (!writer(slot_, replacement_, next_))
    {
        if (AtomicPointerLoad(slot_) != replacement_)
        {
            state_.store(
                HookOwnershipState::ownershipLost,
                std::memory_order_release);
        }
        return false;
    }
    state_.store(HookOwnershipState::restored, std::memory_order_release);
    return true;
}

HookOwnershipState PointerHookOwnership::Health() noexcept
{
    if (state_.load(std::memory_order_acquire) == HookOwnershipState::owned &&
        AtomicPointerLoad(slot_) != replacement_)
    {
        state_.store(HookOwnershipState::ownershipLost, std::memory_order_release);
    }
    return state_.load(std::memory_order_acquire);
}

}  // namespace fo4hdr
