// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuPerformanceQueries.hpp"

#include <cmath>
#include <utility>

namespace fo4hdr {
namespace {

bool QueryMilliseconds(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint,
    UINT64 start, UINT64 end, double& milliseconds) noexcept
{
    if (disjoint.Disjoint) return false;
    if (disjoint.Frequency == 0) return false;
    if (end < start) return false;
    milliseconds = static_cast<double>(end - start) * 1000.0 /
        static_cast<double>(disjoint.Frequency);
    return std::isfinite(milliseconds) && milliseconds >= 0.0;
}

}  // namespace

GpuPerformanceQueries::~GpuPerformanceQueries() noexcept
{
    (void)Retire();
}

bool GpuPerformanceQueries::CreateQuery(ID3D11Device* device, D3D11_QUERY type,
    Microsoft::WRL::ComPtr<ID3D11Query>& query) noexcept
{
    const D3D11_QUERY_DESC description{type, 0};
    if (FAILED(device->CreateQuery(&description, &query))) return false;
    return query.Get() != nullptr;
}

bool GpuPerformanceQueries::CreateSlot(ID3D11Device* device, Slot& slot) noexcept
{
    if (!CreateQuery(device, D3D11_QUERY_TIMESTAMP_DISJOINT, slot.disjoint)) return false;
    if (!CreateQuery(device, D3D11_QUERY_TIMESTAMP, slot.start)) return false;
    return CreateQuery(device, D3D11_QUERY_TIMESTAMP, slot.end);
}

bool GpuPerformanceQueries::PrepareSlots(ID3D11Device* device,
    std::array<Slot, slotCount>& slots) noexcept
{
    for (auto& slot : slots)
        if (!CreateSlot(device, slot)) return false;
    return true;
}

bool GpuPerformanceQueries::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, bool enabled) noexcept
{
    if (initialized_) return false;
    ownerThread_ = std::this_thread::get_id();
    initialized_ = true;
    if (!enabled) return true;
    if (device == nullptr) return false;
    if (immediateContext == nullptr) return false;
    std::array<Slot, slotCount> prepared;
    if (!PrepareSlots(device, prepared)) return false;
    slots_ = std::move(prepared);
    context_ = immediateContext;
    return true;
}

bool GpuPerformanceQueries::OnOwnerThread() const noexcept
{
    return initialized_ && ownerThread_ == std::this_thread::get_id();
}

bool GpuPerformanceQueries::Ready() const noexcept
{
    if (!OnOwnerThread()) return false;
    if (retired_) return false;
    return context_.Get() != nullptr;
}

bool GpuPerformanceQueries::CanBegin(std::uint64_t frame) const noexcept
{
    if (!Ready()) return false;
    if (statistics_.Complete()) return false;
    if (activeSlot_ != slotCount) return false;
    if (frame <= lastFrame_) return false;
    return true;
}

bool GpuPerformanceQueries::Begin(std::uint64_t frame) noexcept
{
    if (!CanBegin(frame)) return false;
    for (std::size_t offset = 0; offset < slotCount; ++offset)
    {
        const auto index = (cursor_ + offset) % slotCount;
        auto& slot = slots_[index];
        if (slot.pending) continue;
        activeSlot_ = index;
        cursor_ = (index + 1) % slotCount;
        lastFrame_ = frame;
        slot.submitted = false;
        context_->Begin(slot.disjoint.Get());
        context_->End(slot.start.Get());
        ++issuedSamples_;
        return true;
    }
    return false;
}

void GpuPerformanceQueries::End(bool submitted) noexcept
{
    if (!Ready()) return;
    if (activeSlot_ == slotCount) return;
    auto& slot = slots_[activeSlot_];
    context_->End(slot.end.Get());
    context_->End(slot.disjoint.Get());
    slot.submitted = submitted;
    slot.pending = true;
    activeSlot_ = slotCount;
}

GpuPerformanceQueries::Observation GpuPerformanceQueries::ReadState(HRESULT result) noexcept
{
    if (result == S_OK) return Observation::ready;
    if (result == S_FALSE) return Observation::pending;
    return Observation::rejected;
}

GpuPerformanceQueries::Observation GpuPerformanceQueries::TimestampState(
    HRESULT start, HRESULT end) noexcept
{
    const auto started = ReadState(start);
    const auto ended = ReadState(end);
    // A failed read discards the sample even when its partner is still pending.
    if (started == Observation::rejected) return Observation::rejected;
    if (ended == Observation::rejected) return Observation::rejected;
    if (started == Observation::pending) return Observation::pending;
    return ended;
}

GpuPerformanceQueries::Observation GpuPerformanceQueries::ReadSlot(
    Slot& slot, double& milliseconds) const noexcept
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    const auto disjointResult = ReadState(context_->GetData(slot.disjoint.Get(), &disjoint,
        sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH));
    if (disjointResult != Observation::ready) return disjointResult;
    UINT64 start{}, end{};
    const auto startResult = context_->GetData(slot.start.Get(), &start,
        sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const auto endResult = context_->GetData(slot.end.Get(), &end,
        sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const auto state = TimestampState(startResult, endResult);
    if (state != Observation::ready) return state;
    if (!slot.submitted) return Observation::rejected;
    if (!QueryMilliseconds(disjoint, start, end, milliseconds)) return Observation::rejected;
    return Observation::ready;
}

void GpuPerformanceQueries::PublishSlot(Slot& slot) noexcept
{
    double milliseconds{};
    const auto result = ReadSlot(slot, milliseconds);
    if (result == Observation::pending) return;
    slot.pending = false;
    if (result == Observation::rejected)
    {
        ++rejectedSamples_;
        return;
    }
    if (statistics_.Complete()) return;
    (void)statistics_.AddSample(milliseconds);
}

void GpuPerformanceQueries::Poll() noexcept
{
    if (!Ready()) return;
    if (activeSlot_ != slotCount) return;
    for (auto& slot : slots_)
        if (slot.pending) PublishSlot(slot);
}

GpuPerformanceSnapshot GpuPerformanceQueries::Snapshot() const noexcept
{
    if (!Ready()) return {};
    GpuPerformanceSnapshot result{
        .available = true,
        .complete = statistics_.Complete(),
        .issuedSamples = issuedSamples_,
        .rejectedSamples = rejectedSamples_,
        .pendingSamples = PendingCount(),
        .warmupSamples = statistics_.WarmupSamples(),
        .measuredSamples = statistics_.MeasuredSamples(),
        .percentiles = statistics_.Percentiles()
    };
    return result;
}

std::size_t GpuPerformanceQueries::PendingCount() const noexcept
{
    std::size_t count{};
    for (const auto& slot : slots_)
        if (slot.pending) ++count;
    return count;
}

bool GpuPerformanceQueries::Retire() noexcept
{
    if (!initialized_) return true;
    if (!OnOwnerThread()) return false;
    if (activeSlot_ != slotCount) return false;
    slots_ = {};
    context_.Reset();
    statistics_.Reset();
    issuedSamples_ = 0;
    rejectedSamples_ = 0;
    lastFrame_ = 0;
    cursor_ = 0;
    retired_ = true;
    return true;
}

}  // namespace fo4hdr
