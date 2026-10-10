// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "../DrawObservation.hpp"

namespace fo4hdr {

struct DrawInvocation
{
    void (*invoke)(void*);
    void* arguments;
    void operator()() const { invoke(arguments); }
};
void ExecuteObservedDraw(DrawObservation&, ID3D11DeviceContext*, const DrawInvocation&) noexcept;

} // namespace fo4hdr
