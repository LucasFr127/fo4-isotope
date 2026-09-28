// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace fo4hdr {

enum class HookOwnershipState
{
    empty,
    installing,
    owned,
    ownershipLost,
    installFailed,
    restored
};

using HookPointerWriter = bool (*)(void** slot, void* expected, void* value) noexcept;
using HookNextPublisher = bool (*)(void* next) noexcept;

[[nodiscard]] inline void* AtomicPointerLoad(
    void* const* slot) noexcept
{
    if (slot == nullptr) return nullptr;
    return std::atomic_ref<void*>(
        *const_cast<void**>(slot)).load(std::memory_order_acquire);
}

template <class Function, class... Arguments>
decltype(auto) InvokeCapturedNext(
    Function next,
    Arguments&&... arguments) noexcept(
        noexcept(next(std::forward<Arguments>(arguments)...)))
{
    return next(std::forward<Arguments>(arguments)...);
}

class PointerHookOwnership
{
public:
    [[nodiscard]] bool Install(
        void** slot,
        void* replacement,
        HookPointerWriter writer,
        HookNextPublisher publishNext = nullptr) noexcept;
    [[nodiscard]] bool Remove(HookPointerWriter writer) noexcept;
    [[nodiscard]] HookOwnershipState Health() noexcept;

    [[nodiscard]] void* Next() const noexcept { return next_; }
    [[nodiscard]] HookOwnershipState State() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

private:
    void** slot_{};
    void* next_{};
    void* replacement_{};
    std::atomic<HookOwnershipState> state_{HookOwnershipState::empty};
};

struct VtablePatch
{
    std::size_t index{};
    void* replacement{};
};

enum class OneShotInitializationState : std::uint32_t
{
    uninitialized,
    installing,
    active,
    failed
};

enum class OneShotInitializationAction : std::uint32_t
{
    install,
    alreadyActive,
    unavailable
};

class OneShotInitialization
{
public:
    [[nodiscard]] OneShotInitializationAction Begin() noexcept;
    void Complete(bool succeeded) noexcept;
    [[nodiscard]] OneShotInitializationState State() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<OneShotInitializationState> state_{
        OneShotInitializationState::uninitialized};
};

class BoundedAtomicFlagGuard
{
public:
    explicit BoundedAtomicFlagGuard(
        std::atomic_flag& flag,
        std::size_t maximumAttempts) noexcept;
    ~BoundedAtomicFlagGuard();

    BoundedAtomicFlagGuard(const BoundedAtomicFlagGuard&) = delete;
    BoundedAtomicFlagGuard& operator=(const BoundedAtomicFlagGuard&) = delete;

    [[nodiscard]] bool OwnsLock() const noexcept { return owns_; }

private:
    std::atomic_flag* flag_{};
    bool owns_{};
};

class ObjectGenerationIdentity
{
public:
    // The owning registry serializes publishers. Publish writes generation
    // before release-publishing object; Matches/Object acquire that publication.
    // Final-release retirement uses a generation-matched compare/exchange, and
    // readers never dereference the stored identity.
    [[nodiscard]] bool Publish(void* object, std::uint64_t generation) noexcept;
    [[nodiscard]] bool Matches(void* object) const noexcept;
    [[nodiscard]] bool Retire(void* object, std::uint64_t generation) noexcept;
    [[nodiscard]] void* Object() const noexcept
    {
        return object_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t Generation() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }

private:
    std::atomic<void*> object_{};
    std::atomic<std::uint64_t> generation_{};
};

[[nodiscard]] bool PrepareShadowVtable(
    std::span<void* const> current,
    std::span<void*> shadow,
    std::span<const VtablePatch> patches) noexcept;

template <class Result, class Call, class OnException>
Result InvokeNoexceptBoundary(
    Result failureResult,
    Call&& call,
    OnException&& onException) noexcept
{
    try
    {
        return std::forward<Call>(call)();
    }
    catch (...)
    {
        std::forward<OnException>(onException)();
        return failureResult;
    }
}

}  // namespace fo4hdr
