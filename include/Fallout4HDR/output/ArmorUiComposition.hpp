// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstdint>

namespace fo4hdr {

enum class ArmorUiOwner : std::uint32_t
{
    none,
    powerArmor,
    other
};

void SetArmorUiCorrectionEnabled(bool enabled) noexcept;
[[nodiscard]] bool ArmorUiCorrectionEnabled() noexcept;
// Native scopes follow correction eligibility. Resets advance the generation
// while preserving that request, so stale scopes cannot start transactions.
[[nodiscard]] bool ArmorUiScopeEnabled() noexcept;
[[nodiscard]] std::uint64_t ArmorUiScopeGeneration() noexcept;
void ResetArmorUiScope() noexcept;

struct ArmorUiScopeToken
{
    ArmorUiOwner owner{ArmorUiOwner::none};
    std::uint32_t threadId{};
    // Nonzero serials are monotonic within thread. Pair with threadId and the
    // shared request generation when validating a scope lifetime.
    std::uint64_t serial{};
    std::uint64_t requestGeneration{};
    std::uint32_t targetArgument{};
    std::uint32_t depth{};
    bool postAa{};
    bool hasRoot{};
};

[[nodiscard]] ArmorUiScopeToken CurrentArmorUiScope() noexcept;

// Fixed-lifetime renderer callback. Each scope retains its entry callback so
// request changes cannot strand an in-progress representation transaction.
using ArmorUiBoundaryCallback = void (*)(const ArmorUiScopeToken&, bool entering) noexcept;
void SetArmorUiBoundaryCallback(ArmorUiBoundaryCallback callback) noexcept;

enum class ArmorUiCompositionExit : std::uint8_t { ignore, decode, invalidate };

struct ArmorUiCompositionExitEvidence
{
    bool active{};
    bool scopeMatches{};
    bool ownerThreadMatches{};
    bool contextSafe{};
    bool generationMatches{};
    bool frameMatches{};
};

// The outer transaction is paired by native thread and serial. Request changes
// do not cancel its cleanup; renderer identity changes always require a black
// handoff instead of decoding through an unqualified immediate context.
[[nodiscard]] constexpr ArmorUiCompositionExit DecideArmorUiCompositionExit(
    const ArmorUiCompositionExitEvidence& evidence) noexcept
{
    if (!evidence.active || !evidence.scopeMatches) return ArmorUiCompositionExit::ignore;
    return evidence.ownerThreadMatches && evidence.contextSafe &&
        evidence.generationMatches && evidence.frameMatches ?
        ArmorUiCompositionExit::decode : ArmorUiCompositionExit::invalidate;
}

struct ArmorUiScopeEntry
{
    ArmorUiOwner owner{ArmorUiOwner::none};
    std::uint32_t threadId{};
    std::uint32_t targetArgument{};
    bool postAa{};
    bool hasRoot{};
};

class ArmorUiScope
{
public:
    static constexpr std::uint32_t maximumDepth = 32;

    explicit ArmorUiScope(const ArmorUiScopeEntry& entry) noexcept;
    ~ArmorUiScope();

    ArmorUiScope(const ArmorUiScope&) = delete;
    ArmorUiScope& operator=(const ArmorUiScope&) = delete;
    ArmorUiScope(ArmorUiScope&&) = delete;
    ArmorUiScope& operator=(ArmorUiScope&&) = delete;

    [[nodiscard]] bool Entered() const noexcept { return entered_; }
    [[nodiscard]] bool NestingRejected() const noexcept
    {
        return nestingRejected_;
    }

private:
    void MaskRejectedNestedBoundary(std::uint32_t threadId, std::uint64_t requestState) noexcept;

    ArmorUiScopeToken previous_{};
    ArmorUiScopeToken token_{};
    ArmorUiBoundaryCallback boundary_{};
    bool entered_{};
    bool nestingRejected_{};
};

}  // namespace fo4hdr
