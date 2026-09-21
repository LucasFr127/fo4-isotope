// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fo4hdr {

enum class HookOwnershipState
{
    empty,
    installing,
    owned,
    ownershipLost,
    installFailed,
    restored,
    removing,
    passive
};

using HookPointerWriter = bool (*)(void** slot, void* expected, void* value) noexcept;
using HookNextPublisher = bool (*)(void* next) noexcept;

[[nodiscard]] void* AtomicPointerLoad(void* const* slot) noexcept;
// Executable committed storage is only one prerequisite; the caller proves ABI/lifetime.
[[nodiscard]] bool IsExecutableHookTarget(void* target) noexcept;

using HookPageProtector = bool (*)(void* address, std::size_t bytes,
    std::uint32_t protection, std::uint32_t& previous) noexcept;
using HookPointerExchange = void* (*)(void** slot, void* expected, void* value) noexcept;

struct HookPointerWriteOperations
{
    HookPageProtector protect{};
    HookPointerExchange exchange{};
};

// The injected operations obey the same page-protection and expected-value contracts
// as the native adapter. A false result can leave value reachable after failed rollback.
// The caller keeps storage mapped and serializes protection changes on its page.
[[nodiscard]] bool WriteProtectedPointer(void** slot, void* expected, void* value,
    const HookPointerWriteOperations& operations) noexcept;
// The native adapter refuses executable slot pages before changing any protection.
[[nodiscard]] bool WriteProtectedPointer(void** slot, void* expected, void* value) noexcept;

// The caller supplies stable, readable aligned storage, proves ABI, and keeps next code resident.
// Published callback state and this record outlive every reachable or in-flight callback.
// Neither a false result nor successful removal establishes safe destruction/unloading.
class PointerHookOwnership
{
public:
    [[nodiscard]] bool Install(void** slot, void* replacement, HookPointerWriter writer,
        HookNextPublisher publishNext) noexcept;
    [[nodiscard]] bool Remove(HookPointerWriter writer) noexcept;
    [[nodiscard]] HookOwnershipState Health() noexcept;

    [[nodiscard]] void* Next() const noexcept
    {
        return next_.load(std::memory_order_acquire);
    }
    [[nodiscard]] HookOwnershipState State() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] bool Capture(void** slot, void* replacement) noexcept;
    void RecordWriteFailure(HookOwnershipState absentState) noexcept;

    void** slot_{};
    std::atomic<void*> next_{};
    void* replacement_{};
    std::atomic<HookOwnershipState> state_{HookOwnershipState::empty};
};

struct VtablePatch
{
    std::size_t index{};
    void* replacement{};
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


[[nodiscard]] std::size_t DiscoverCallableVtableSlots(void* const* table,
    std::size_t minimum, std::size_t maximum) noexcept;

}  // namespace fo4hdr
