// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/PerformanceStatistics.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace fo4hdr {

struct GpuPerformanceSnapshot
{
    bool available{};
    bool complete{};
    std::uint64_t issuedSamples{};
    std::uint64_t rejectedSamples{};
    std::size_t pendingSamples{};
    std::size_t warmupSamples{};
    std::size_t measuredSamples{};
    PerformancePercentiles percentiles;
};

// One qualified device/context epoch; serialized initialization-thread calls.
// Begin/End bracket only owned terminal work, with every foreign query excluded.
// Destruction and retirement require no active interval on this same thread.
class GpuPerformanceQueries
{
public:
    GpuPerformanceQueries() noexcept = default;
    ~GpuPerformanceQueries() noexcept;
    GpuPerformanceQueries(const GpuPerformanceQueries&) = delete;
    GpuPerformanceQueries& operator=(const GpuPerformanceQueries&) = delete;

    // The presenting owner has positively qualified this same-device immediate pair.
    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, bool enabled) noexcept;
    [[nodiscard]] bool Begin(std::uint64_t frame) noexcept;
    void End(bool submitted) noexcept;
    void Poll() noexcept;
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] GpuPerformanceSnapshot Snapshot() const noexcept;

private:
    static constexpr std::size_t slotCount = 8;
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> start;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending{};
        bool submitted{};
    };

    enum class Observation { pending, rejected, ready };
    [[nodiscard]] static bool CreateQuery(ID3D11Device* device, D3D11_QUERY type,
        Microsoft::WRL::ComPtr<ID3D11Query>& query) noexcept;
    [[nodiscard]] static bool CreateSlot(ID3D11Device* device, Slot& slot) noexcept;
    [[nodiscard]] static bool PrepareSlots(ID3D11Device* device,
        std::array<Slot, slotCount>& slots) noexcept;
    [[nodiscard]] static Observation ReadState(HRESULT result) noexcept;
    [[nodiscard]] static Observation TimestampState(HRESULT start, HRESULT end) noexcept;
    [[nodiscard]] Observation ReadSlot(Slot& slot, double& milliseconds) const noexcept;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] bool CanBegin(std::uint64_t frame) const noexcept;
    [[nodiscard]] std::size_t PendingCount() const noexcept;
    void PublishSlot(Slot& slot) noexcept;

    std::array<Slot, slotCount> slots_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::thread::id ownerThread_;
    FixedPerformanceStatistics statistics_;
    std::size_t cursor_{};
    std::size_t activeSlot_{slotCount};
    std::uint64_t lastFrame_{};
    std::uint64_t issuedSamples_{};
    std::uint64_t rejectedSamples_{};
    bool initialized_{};
    bool retired_{};
};

}  // namespace fo4hdr
