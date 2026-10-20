// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#include <cstdint>
#include <cstring>
namespace fo4hdr::game {
namespace detail {
template <class PooledString>
bool SameNativeRendererKey(const PooledString& name, const PooledString& canonical) noexcept
{
    static_assert(sizeof(PooledString) == sizeof(void*));
    const void* entry{};
    std::memcpy(&entry, &canonical, sizeof(entry));
    // Native renderer lookup compares pooled entries without following string leaves.
    return entry != nullptr && std::memcmp(&name, &canonical, sizeof(name)) == 0;
}
}
// Called once during validated F4SE plugin initialization, before rendering.
bool InitializeArmorUiObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept;
bool InitializeScreenUiObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept;
}
