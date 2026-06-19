// SPDX-License-Identifier: GPL-3.0-or-later

#include "SwapChainObservation.hpp"

#include "RendererLifetime.hpp"

#include "Fallout4HDR/hooks/HookOwnership.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace fo4hdr {
namespace {

using ReleaseFunction = ULONG (STDMETHODCALLTYPE*)(IDXGISwapChain*);
using PresentFunction = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFunction = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
    DXGI_FORMAT, UINT);
using DescriptionFunction = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);

constexpr auto terminalToken = RendererLifetime::terminalEpoch;

struct ChainRecord
{
    IDXGISwapChain* chain{};
    void** table{};
    HWND window{};
    std::array<void*, 18> original{};
    ReleaseFunction release{};
    PresentFunction present{};
    ResizeFunction resize{};
    DescriptionFunction description{};
    PointerHookOwnership releaseOwner;
    PointerHookOwnership presentOwner;
    PointerHookOwnership resizeOwner;
    RendererLifetime lifetime;
    std::atomic<bool> offered{};
};

// Records never recycle: a removed slot does not prove that its callback has returned.
std::array<ChainRecord, 2> records;
std::size_t recordCount{};
std::atomic<ChainRecord*> selected{};
thread_local bool observing{};

class ObservationScope
{
public:
    ObservationScope() noexcept : entered_(!observing)
    {
        observing = true;
    }
    ~ObservationScope()
    {
        if (entered_)
        {
            observing = false;
        }
    }
    [[nodiscard]] bool Entered() const noexcept { return entered_; }

private:
    bool entered_;
};

bool SameTable(const ChainRecord& record, IDXGISwapChain* chain) noexcept
{
    if (chain != record.chain)
    {
        return false;
    }
    void** table{};
    std::memcpy(&table, chain, sizeof(table));
    return table == record.table;
}

bool SameDescription(const ChainRecord& record) noexcept
{
    if (!SameTable(record, record.chain))
    {
        return false;
    }
    if (AtomicPointerLoad(&record.table[12]) != record.original[12])
    {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC description{};
    return SUCCEEDED(record.description(record.chain, &description)) &&
        description.OutputWindow == record.window;
}

bool SlotsOwned(ChainRecord& record) noexcept
{
    return record.releaseOwner.Health() == HookOwnershipState::owned &&
        record.presentOwner.Health() == HookOwnershipState::owned &&
        record.resizeOwner.Health() == HookOwnershipState::owned;
}

std::uint64_t ObserveEntry(ChainRecord& record, IDXGISwapChain* chain) noexcept
{
    const auto token = record.lifetime.CurrentEpoch();
    if (selected.load(std::memory_order_acquire) != &record)
    {
        record.lifetime.Retire();
        return terminalToken;
    }
    if (!SameTable(record, chain) || !SlotsOwned(record))
    {
        record.lifetime.Retire();
        return terminalToken;
    }
    return token;
}

void RetireDeviceLoss(ChainRecord& record, HRESULT result) noexcept
{
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        // Resize cannot recreate this device; loss invalidates even a later same-record epoch.
        record.lifetime.Retire();
    }
}

void CompleteResize(ChainRecord& record, std::uint64_t transition, HRESULT result) noexcept
{
    if (transition == terminalToken || FAILED(result))
    {
        return;
    }
    if (!SlotsOwned(record) || !SameDescription(record))
    {
        return;
    }
    record.lifetime.CompleteResize(transition);
}

template <std::size_t Index>
ULONG STDMETHODCALLTYPE HookRelease(IDXGISwapChain* chain) noexcept
{
    auto& record = records[Index];
    const auto next = record.release;
    ObservationScope scope;
    if (scope.Entered())
    {
        static_cast<void>(ObserveEntry(record, chain));
    }
    const auto count = next(chain);
    // Zero can destroy the receiver and its table; touch only permanent record storage now.
    if (count == 0)
    {
        record.lifetime.Retire();
    }
    return count;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT interval, UINT flags) noexcept
{
    auto& record = records[Index];
    const auto next = record.present;
    ObservationScope scope;
    if (scope.Entered())
    {
        static_cast<void>(ObserveEntry(record, chain));
    }
    const auto result = next(chain, interval, flags);
    RetireDeviceLoss(record, result);
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* chain, UINT count, UINT width,
    UINT height, DXGI_FORMAT format, UINT flags) noexcept
{
    auto& record = records[Index];
    const auto next = record.resize;
    ObservationScope scope;
    const auto token = scope.Entered() ? ObserveEntry(record, chain) : terminalToken;
    const auto transition = scope.Entered() ? record.lifetime.BeginResize(token) : terminalToken;
    const auto result = next(chain, count, width, height, format, flags);
    RetireDeviceLoss(record, result);
    if (scope.Entered())
    {
        CompleteResize(record, transition, result);
    }
    return result;
}

template <std::size_t Index>
bool PublishRelease(void* next) noexcept
{
    return next == reinterpret_cast<void*>(records[Index].release);
}

template <std::size_t Index>
bool PublishPresent(void* next) noexcept
{
    return next == reinterpret_cast<void*>(records[Index].present);
}

template <std::size_t Index>
bool PublishResize(void* next) noexcept
{
    return next == reinterpret_cast<void*>(records[Index].resize);
}

bool WriteSlot(void** slot, void* expected, void* value) noexcept
{
    return WriteProtectedPointer(slot, expected, value);
}

void RollBack(ChainRecord& record) noexcept
{
    record.lifetime.Retire();
    static_cast<void>(record.resizeOwner.Remove(WriteSlot));
    static_cast<void>(record.presentOwner.Remove(WriteSlot));
    static_cast<void>(record.releaseOwner.Remove(WriteSlot));
}

template <std::size_t Index>
bool InstallSlots(ChainRecord& record) noexcept
{
    if (!record.releaseOwner.Install(&record.table[2], reinterpret_cast<void*>(&HookRelease<Index>),
            WriteSlot, PublishRelease<Index>))
    {
        return false;
    }
    if (!record.presentOwner.Install(&record.table[8], reinterpret_cast<void*>(&HookPresent<Index>),
            WriteSlot, PublishPresent<Index>))
    {
        return false;
    }
    return record.resizeOwner.Install(&record.table[13], reinterpret_cast<void*>(&HookResize<Index>),
        WriteSlot, PublishResize<Index>);
}

bool CallableTable(std::span<void*, 18> table) noexcept
{
    for (const auto entry : table)
    {
        if (!IsExecutableHookTarget(entry))
        {
            return false;
        }
    }
    return true;
}

bool PrepareRecord(ChainRecord& record, IDXGISwapChain* chain,
    std::span<void*, 18> table, HWND window) noexcept
{
    record.chain = chain;
    record.table = table.data();
    record.window = window;
    std::copy(table.begin(), table.end(), record.original.begin());
    record.release = reinterpret_cast<ReleaseFunction>(table[2]);
    record.present = reinterpret_cast<PresentFunction>(table[8]);
    record.resize = reinterpret_cast<ResizeFunction>(table[13]);
    record.description = reinterpret_cast<DescriptionFunction>(table[12]);
    if (!SameTable(record, chain))
    {
        return false;
    }
    // Each never-reused record owns a disjoint epoch interval, including inactive transitions.
    return record.lifetime.Prepare((recordCount + 1) << 32, (recordCount + 2) << 32);
}

}  // namespace

bool SelectOwnedSwapChain(IDXGISwapChain* chain, std::span<void*, 18> table, HWND window) noexcept
{
    if (chain == nullptr || window == nullptr)
    {
        return false;
    }
    if (recordCount == records.size())
    {
        return false;
    }
    if (ReadSwapChainObservation().active || !CallableTable(table))
    {
        return false;
    }
    auto& record = records[recordCount];
    if (!PrepareRecord(record, chain, table, window))
    {
        return false;
    }
    if (auto* previous = selected.load(std::memory_order_acquire))
    {
        previous->lifetime.Retire();
    }
    ++recordCount;
    selected.store(&record, std::memory_order_release);
    return true;
}

void OfferCreatedSwapChain(IDXGISwapChain* chain) noexcept
{
    auto* record = selected.load(std::memory_order_acquire);
    if (record == nullptr || chain != record->chain)
    {
        return;
    }
    if (record->offered.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    if (!SameDescription(*record))
    {
        record->lifetime.Retire();
        return;
    }
    const auto installed = record == &records[0] ? InstallSlots<0>(*record) : InstallSlots<1>(*record);
    if (!installed)
    {
        RollBack(*record);
        return;
    }
    record->lifetime.PublishInitial();
}

SwapChainObservation ReadSwapChainObservation() noexcept
{
    const auto* record = selected.load(std::memory_order_acquire);
    if (record == nullptr)
    {
        return {};
    }
    const auto token = record->lifetime.CurrentEpoch();
    return {token, (token & 1) != 0};
}

}  // namespace fo4hdr
