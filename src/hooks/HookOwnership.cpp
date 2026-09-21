// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/HookOwnership.hpp"

#include <Windows.h>
#include <algorithm>

namespace fo4hdr {
namespace {

bool IsAlignedSlot(void* const* slot) noexcept
{
    if (slot == nullptr)
    {
        return false;
    }
    return reinterpret_cast<std::uintptr_t>(slot) % std::atomic_ref<void*>::required_alignment == 0;
}

bool ReadPage(void* address, MEMORY_BASIC_INFORMATION& page) noexcept
{
    if (VirtualQuery(address, &page, sizeof(page)) != sizeof(page))
    {
        return false;
    }
    if (page.State != MEM_COMMIT)
    {
        return false;
    }
    return (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

bool ContainsPointer(const MEMORY_BASIC_INFORMATION& page, void** slot) noexcept
{
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    const auto base = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    if (address < base || address - base > page.RegionSize)
    {
        return false;
    }
    return sizeof(void*) <= page.RegionSize - (address - base);
}

bool IsDataSlotProtection(DWORD protection) noexcept
{
    switch (protection & 0xFF)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool ReadableSlot(void** slot) noexcept
{
    MEMORY_BASIC_INFORMATION page{};
    if (!ReadPage(slot, page))
    {
        return false;
    }
    if (!IsDataSlotProtection(page.Protect))
    {
        return false;
    }
    return ContainsPointer(page, slot);
}

bool ProtectPage(void* address, std::size_t bytes, std::uint32_t protection,
    std::uint32_t& previous) noexcept
{
    DWORD old{};
    const auto changed = VirtualProtect(address, bytes, protection, &old) != FALSE;
    previous = old;
    return changed;
}

void* ExchangePointer(void** slot, void* expected, void* value) noexcept
{
    std::atomic_ref<void*> pointer(*slot);
    static_cast<void>(pointer.compare_exchange_strong(expected, value, std::memory_order_acq_rel));
    return expected;
}

bool RestoreProtection(void** slot, std::uint32_t previous,
    const HookPointerWriteOperations& operations) noexcept
{
    std::uint32_t discarded{};
    return operations.protect(slot, sizeof(void*), previous, discarded);
}

}  // namespace

void* AtomicPointerLoad(void* const* slot) noexcept
{
    if (!IsAlignedSlot(slot))
    {
        return nullptr;
    }
    return std::atomic_ref<void*>(*const_cast<void**>(slot)).load(std::memory_order_acquire);
}

bool IsExecutableHookTarget(void* target) noexcept
{
    if (target == nullptr)
    {
        return false;
    }
    MEMORY_BASIC_INFORMATION page{};
    if (!ReadPage(target, page))
    {
        return false;
    }
    switch (page.Protect & 0xFF)
    {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool WriteProtectedPointer(void** slot, void* expected, void* value,
    const HookPointerWriteOperations& operations) noexcept
{
    if (!IsAlignedSlot(slot))
    {
        return false;
    }
    if (operations.protect == nullptr || operations.exchange == nullptr)
    {
        return false;
    }
    std::uint32_t previous{};
    if (!operations.protect(slot, sizeof(void*), PAGE_READWRITE, previous))
    {
        return false;
    }
    const auto observed = operations.exchange(slot, expected, value);
    if (RestoreProtection(slot, previous, operations))
    {
        return observed == expected;
    }
    // Restore only our value; a displaced foreign owner is never overwritten.
    if (observed == expected)
    {
        static_cast<void>(operations.exchange(slot, value, expected));
    }
    static_cast<void>(RestoreProtection(slot, previous, operations));
    return false;
}

bool WriteProtectedPointer(void** slot, void* expected, void* value) noexcept
{
    if (!IsAlignedSlot(slot) || !ReadableSlot(slot))
    {
        return false;
    }
    const HookPointerWriteOperations operations{ProtectPage, ExchangePointer};
    return WriteProtectedPointer(slot, expected, value, operations);
}

bool PointerHookOwnership::Capture(void** slot, void* replacement) noexcept
{
    if (!IsAlignedSlot(slot) || replacement == nullptr)
    {
        return false;
    }
    const auto observed = AtomicPointerLoad(slot);
    if (observed == nullptr || observed == replacement)
    {
        return false;
    }
    if (!IsExecutableHookTarget(observed))
    {
        return false;
    }
    slot_ = slot;
    replacement_ = replacement;
    next_.store(observed, std::memory_order_release);
    return true;
}

bool PointerHookOwnership::Install(void** slot, void* replacement, HookPointerWriter writer,
    HookNextPublisher publishNext) noexcept
{
    auto expectedState = HookOwnershipState::empty;
    if (!state_.compare_exchange_strong(expectedState, HookOwnershipState::installing,
            std::memory_order_acq_rel))
    {
        return false;
    }
    if (writer == nullptr || publishNext == nullptr)
    {
        state_.store(HookOwnershipState::installFailed, std::memory_order_release);
        return false;
    }
    if (!Capture(slot, replacement) || !publishNext(Next()))
    {
        state_.store(HookOwnershipState::installFailed, std::memory_order_release);
        return false;
    }
    if (!writer(slot_, Next(), replacement_))
    {
        RecordWriteFailure(HookOwnershipState::installFailed);
        return false;
    }
    state_.store(HookOwnershipState::owned, std::memory_order_release);
    return true;
}

void PointerHookOwnership::RecordWriteFailure(HookOwnershipState absentState) noexcept
{
    const auto state = AtomicPointerLoad(slot_) == replacement_ ?
        HookOwnershipState::passive : absentState;
    state_.store(state, std::memory_order_release);
}

bool PointerHookOwnership::Remove(HookPointerWriter writer) noexcept
{
    if (writer == nullptr)
    {
        return false;
    }
    auto expected = HookOwnershipState::owned;
    if (!state_.compare_exchange_strong(expected, HookOwnershipState::removing,
            std::memory_order_acq_rel))
    {
        return false;
    }
    if (!writer(slot_, replacement_, Next()))
    {
        RecordWriteFailure(HookOwnershipState::ownershipLost);
        return false;
    }
    state_.store(HookOwnershipState::restored, std::memory_order_release);
    return true;
}

HookOwnershipState PointerHookOwnership::Health() noexcept
{
    auto expected = HookOwnershipState::owned;
    if (state_.load(std::memory_order_acquire) != expected)
    {
        return State();
    }
    if (AtomicPointerLoad(slot_) != replacement_)
    {
        static_cast<void>(state_.compare_exchange_strong(expected,
            HookOwnershipState::ownershipLost, std::memory_order_acq_rel));
    }
    return State();
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


namespace {
bool CallableVtableSlot(void* const* slot) noexcept
{
    MEMORY_BASIC_INFORMATION page{};
    if (!ReadPage(const_cast<void**>(slot), page)) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    const auto start = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    if (address < start || address - start > page.RegionSize) return false;
    if (sizeof(void*) > page.RegionSize - (address - start)) return false;
    return IsExecutableHookTarget(AtomicPointerLoad(slot));
}
}  // namespace

namespace {
std::size_t CallableTailLength(void* const* table, std::size_t maximum) noexcept
{
    std::size_t count{};
    for (; count < maximum; ++count)
        if (!CallableVtableSlot(table + count)) break;
    return count;
}

bool ValidVtableBounds(std::size_t minimum, std::size_t maximum) noexcept
{
    return minimum != 0 && minimum <= maximum;
}
}  // namespace

std::size_t DiscoverCallableVtableSlots(void* const* table,
    std::size_t minimum, std::size_t maximum) noexcept
{
    if (table == nullptr) return 0;
    if (!ValidVtableBounds(minimum, maximum)) return 0;
    const auto count = CallableTailLength(table, maximum);
    // The callable tail is a driver-storage heuristic, not proof of private extent.
    if (count == maximum && CallableVtableSlot(table + count)) return 0;
    return count >= minimum ? count : 0;
}

}  // namespace fo4hdr
