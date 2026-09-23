// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../DrawObservation.hpp"

namespace fo4hdr {

struct DrawInvocation
{
    void (*invoke)(void*);
    void* arguments;
    void operator()() const { invoke(arguments); }
};
void ExecuteObservedDraw(DrawObservation&, ID3D11DeviceContext*, bool exactIndexedQuad, const DrawInvocation&) noexcept;

} // namespace fo4hdr
