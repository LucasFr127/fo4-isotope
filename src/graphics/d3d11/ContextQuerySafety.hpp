// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace fo4hdr {

// Timing queries intentionally include plugin work. Count/statistics queries
// must never see the additional private compute dispatch.
[[nodiscard]] constexpr bool IsComputeSensitiveQuery(D3D11_QUERY query) noexcept
{
    constexpr std::array sensitiveQueries{
        D3D11_QUERY_OCCLUSION,
        D3D11_QUERY_PIPELINE_STATISTICS,
        D3D11_QUERY_OCCLUSION_PREDICATE,
        D3D11_QUERY_SO_STATISTICS,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE,
        D3D11_QUERY_SO_STATISTICS_STREAM0,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0,
        D3D11_QUERY_SO_STATISTICS_STREAM1,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1,
        D3D11_QUERY_SO_STATISTICS_STREAM2,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,
        D3D11_QUERY_SO_STATISTICS_STREAM3,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3
    };
    return std::find(sensitiveQueries.begin(), sensitiveQueries.end(), query) !=
        sensitiveQueries.end();
}

// Context-lifetime bookkeeping: these opaque keys are never dereferenced or
// owned. The caller must keep the API object alive for its Begin/End scope.
// Broken or unobserved scopes disable injected compute work until context retirement.
struct ContextQuerySafety
{
    static constexpr std::size_t capacity = 32;
    std::array<const void*, capacity> active{};
    bool unknown{};
    std::size_t executionDepth{};

    [[nodiscard]] bool EnterCommandList() noexcept;

    void LeaveCommandList() noexcept;

    void Begin(const void* key) noexcept;

    void End(const void* key) noexcept;

    [[nodiscard]] bool CanDispatch() const noexcept;
};

}  // namespace fo4hdr
