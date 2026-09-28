// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "FO4Isotope/hooks/HookOwnership.hpp"

#include <algorithm>

namespace fo4hdr {

OneShotInitializationAction OneShotInitialization::Begin() noexcept
{
    auto expected = OneShotInitializationState::uninitialized;
    if (state_.compare_exchange_strong(
            expected,
            OneShotInitializationState::installing,
            std::memory_order_acq_rel))
    {
        return OneShotInitializationAction::install;
    }
    return expected == OneShotInitializationState::active ?
        OneShotInitializationAction::alreadyActive :
        OneShotInitializationAction::unavailable;
}

void OneShotInitialization::Complete(bool succeeded) noexcept
{
    auto expected = OneShotInitializationState::installing;
    (void)state_.compare_exchange_strong(
        expected,
        succeeded ? OneShotInitializationState::active :
                    OneShotInitializationState::failed,
        std::memory_order_acq_rel);
}

BoundedAtomicFlagGuard::BoundedAtomicFlagGuard(
    std::atomic_flag& flag,
    std::size_t maximumAttempts) noexcept :
    flag_(&flag)
{
    for (std::size_t attempt = 0; attempt < maximumAttempts; ++attempt)
    {
        if (!flag_->test_and_set(std::memory_order_acquire))
        {
            owns_ = true;
            break;
        }
    }
}

BoundedAtomicFlagGuard::~BoundedAtomicFlagGuard()
{
    if (owns_)
    {
        flag_->clear(std::memory_order_release);
    }
}

bool ObjectGenerationIdentity::Publish(
    void* object,
    std::uint64_t generation) noexcept
{
    if (object == nullptr || generation == 0) return false;
    if (object_.load(std::memory_order_acquire) != nullptr) return false;
    auto expected = static_cast<void*>(nullptr);
    generation_.store(generation, std::memory_order_relaxed);
    return object_.compare_exchange_strong(
        expected,
        object,
        std::memory_order_release,
        std::memory_order_relaxed);
}

bool ObjectGenerationIdentity::Matches(void* object) const noexcept
{
    return object != nullptr &&
           object_.load(std::memory_order_acquire) == object;
}

bool ObjectGenerationIdentity::Retire(
    void* object,
    std::uint64_t generation) noexcept
{
    if (object == nullptr || generation == 0) return false;
    if (generation_.load(std::memory_order_acquire) != generation)
        return false;
    return object_.compare_exchange_strong(
        object,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

bool PrepareShadowVtable(
    std::span<void* const> current,
    std::span<void*> shadow,
    std::span<const VtablePatch> patches) noexcept
{
    if (current.empty() || patches.empty()) return false;
    if (shadow.size() != current.size()) return false;
    const auto canApply = [current](const VtablePatch& patch) noexcept {
        return patch.index < current.size() && patch.replacement != nullptr &&
            current[patch.index] != patch.replacement;
    };
    if (!std::all_of(patches.begin(), patches.end(), canApply))
        return false;
    std::copy(current.begin(), current.end(), shadow.begin());
    for (const auto& patch : patches) shadow[patch.index] = patch.replacement;
    return true;
}

}  // namespace fo4hdr
