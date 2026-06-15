// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fo4hdr {

struct DeviceCreationObservation
{
    std::uint32_t flags{};
    bool swapChainReturned{};
    bool deviceReturned{};
    bool contextReturned{};
};

// Only copied creation facts cross this boundary; no COM identity or ownership is retained.
using DeviceCreationObserver = bool (*)(const DeviceCreationObservation& observation);

}  // namespace fo4hdr
