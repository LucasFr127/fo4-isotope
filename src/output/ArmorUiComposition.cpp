// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "Fallout4HDR/output/ArmorUiComposition.hpp"

#include <atomic>
#include <limits>

namespace {

constexpr std::uint64_t kCorrectionBit = 1;
constexpr std::uint64_t kRequestBits = kCorrectionBit;
constexpr std::uint32_t kGenerationShift = 1;
constexpr std::uint64_t kInitialRequestState =
    std::uint64_t{1} << kGenerationShift;

std::atomic<std::uint64_t> g_requestState{kInitialRequestState};
std::atomic<fo4hdr::ArmorUiBoundaryCallback> g_boundary{};
thread_local fo4hdr::ArmorUiScopeToken g_currentScope{};
thread_local std::uint64_t g_nextScopeSerial{1};

[[nodiscard]] constexpr bool RequestEnabled(
    std::uint64_t state,
    std::uint64_t bit) noexcept
{
    return (state & bit) != 0;
}

[[nodiscard]] constexpr std::uint64_t RequestGeneration(
    std::uint64_t state) noexcept
{
    return state >> kGenerationShift;
}

[[nodiscard]] constexpr std::uint64_t NextRequestState(
    std::uint64_t current,
    std::uint64_t requestBits) noexcept
{
    constexpr auto maximumGeneration =
        std::numeric_limits<std::uint64_t>::max() >> kGenerationShift;
    const auto generation = RequestGeneration(current);
    const auto nextGeneration = generation == maximumGeneration
        ? std::uint64_t{1}
        : generation + 1;
    return (nextGeneration << kGenerationShift) | (requestBits & kRequestBits);
}

void ChangeRequestState(std::uint64_t bit, bool enabled) noexcept
{
    auto current = g_requestState.load(std::memory_order_acquire);
    for (;;)
    {
        if (RequestEnabled(current, bit) == enabled)
        {
            return;
        }
        const auto requestBits = enabled
            ? current | bit
            : current & ~bit;
        const auto next = NextRequestState(current, requestBits);
        if (g_requestState.compare_exchange_weak(
                current,
                next,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }
    }
}

void AdvanceRequestGeneration() noexcept
{
    auto current = g_requestState.load(std::memory_order_acquire);
    for (;;)
    {
        const auto next = NextRequestState(current, current);
        if (g_requestState.compare_exchange_weak(
                current,
                next,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }
    }
}

[[nodiscard]] std::uint64_t NextScopeSerial() noexcept
{
    const auto serial = g_nextScopeSerial++;
    if (serial != 0)
    {
        return serial;
    }
    return g_nextScopeSerial++;
}

}  // namespace

namespace fo4hdr {

void SetArmorUiCorrectionEnabled(bool enabled) noexcept
{
    ChangeRequestState(kCorrectionBit, enabled);
}

bool ArmorUiCorrectionEnabled() noexcept
{
    return RequestEnabled(
        g_requestState.load(std::memory_order_acquire),
        kCorrectionBit);
}

bool ArmorUiScopeEnabled() noexcept
{
    return (g_requestState.load(std::memory_order_acquire) & kRequestBits) != 0;
}

std::uint64_t ArmorUiScopeGeneration() noexcept
{
    return RequestGeneration(g_requestState.load(std::memory_order_acquire));
}

void ResetArmorUiScope() noexcept
{
    AdvanceRequestGeneration();
}

ArmorUiScopeToken CurrentArmorUiScope() noexcept
{
    return g_currentScope;
}

void SetArmorUiBoundaryCallback(ArmorUiBoundaryCallback callback) noexcept
{
    g_boundary.store(callback, std::memory_order_release);
}

void ArmorUiScope::MaskRejectedNestedBoundary(
    std::uint32_t threadId, std::uint64_t requestState) noexcept
{
    if (previous_.serial == 0) return;
    token_ = {.owner = ArmorUiOwner::none, .threadId = threadId,
        .serial = NextScopeSerial(), .requestGeneration = RequestGeneration(requestState),
        .depth = previous_.depth + 1};
    boundary_ = g_boundary.load(std::memory_order_acquire);
    if (boundary_) boundary_(token_, true);
}

ArmorUiScope::ArmorUiScope(const ArmorUiScopeEntry& entry) noexcept
    : previous_(g_currentScope)
{
    const auto [owner, threadId, targetArgument, postAa, hasRoot] = entry;
    // Every attempted boundary masks its caller until destruction. This also
    // prevents a disabled nested native call from inheriting an active owner.
    g_currentScope = {};

    const auto requestState =
        g_requestState.load(std::memory_order_acquire);
    const bool validOwner = owner != ArmorUiOwner::none && threadId != 0;
    if ((requestState & kRequestBits) == 0 || !validOwner)
    {
        MaskRejectedNestedBoundary(threadId, requestState);
        return;
    }

    // The published scope is either an entered token or the zeroed mask above.
    const auto nestedDepth = previous_.depth + 1;
    const bool tooDeep = previous_.depth >= maximumDepth || nestedDepth == 0;
    if (previous_.serial != 0 && tooDeep)
    {
        nestingRejected_ = true;
        MaskRejectedNestedBoundary(threadId, requestState);
        return;
    }

    g_currentScope = {
        .owner = owner,
        .threadId = threadId,
        .serial = NextScopeSerial(),
        .requestGeneration = RequestGeneration(requestState),
        .targetArgument = targetArgument,
        .depth = nestedDepth,
        .postAa = postAa,
        .hasRoot = hasRoot,
    };
    entered_ = true;
    token_ = g_currentScope;
    boundary_ = g_boundary.load(std::memory_order_acquire);
    if (boundary_) boundary_(token_, true);
}

ArmorUiScope::~ArmorUiScope()
{
    if (boundary_) boundary_(token_, false);
    g_currentScope = previous_;
}

}  // namespace fo4hdr
