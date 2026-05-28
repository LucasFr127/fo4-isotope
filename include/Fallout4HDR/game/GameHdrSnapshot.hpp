// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <type_traits>

namespace fo4hdr {

struct GameHdrSnapshot
{
    std::uint64_t generation{};
    bool rendererInitialized{};
    bool rendererBorderless{};
    bool rendererVsync{};

    // Opaque identities carry no ownership and must not be dereferenced.
    std::uint64_t rendererDeviceIdentity{};
    std::uint64_t rendererContextIdentity{};
};

static_assert(std::is_trivially_copyable_v<GameHdrSnapshot>);

}  // namespace fo4hdr
