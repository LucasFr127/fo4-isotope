// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstdint>

namespace fo4hdr {

enum class ArmorUiOwner : std::uint32_t { none, powerArmor, other };

struct ArmorUiScopeEntry
{
    ArmorUiOwner owner{ArmorUiOwner::none};
    std::uint32_t threadId{};
    std::uint32_t targetArgument{};
    bool postAa{};
    bool hasRoot{};

    bool operator==(const ArmorUiScopeEntry&) const noexcept = default;
};

struct ArmorUiScopeToken
{
    ArmorUiScopeEntry entry;
    std::uint64_t serial{};
    bool nested{};
    bool aborted{};

    bool operator==(const ArmorUiScopeToken&) const noexcept = default;
};

struct ArmorUiBoundary
{
    void (*notify)(void*, const ArmorUiScopeToken&, bool entering) noexcept{};
    void* state{};
};

// Publish only a fixed-lifetime binding. Detaching prevents new scopes but each
// entered scope retains its binding until exit; retirement must drain callers.
void SetArmorUiBoundary(const ArmorUiBoundary* boundary) noexcept;

class ArmorUiScope
{
public:
    explicit ArmorUiScope(const ArmorUiScopeEntry& entry) noexcept;
    ~ArmorUiScope();
    ArmorUiScope(const ArmorUiScope&) = delete;
    ArmorUiScope& operator=(const ArmorUiScope&) = delete;

private:
    const ArmorUiBoundary* boundary_{};
    ArmorUiScopeToken token_;
    bool previousActive_{};
    int exceptionsOnEntry_{};
};

}  // namespace fo4hdr
