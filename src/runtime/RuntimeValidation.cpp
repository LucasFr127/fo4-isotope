// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/runtime/RuntimeValidation.hpp"

namespace fo4hdr {

RuntimeValidationResult ValidateRuntimeIdentity(
    const RuntimeIdentity& observed) noexcept
{
    if (observed.runtimeVersion != RuntimeVersion{1, 10, 163, 0})
    {
        return {false, RuntimeValidationReason::RuntimeVersionMismatch};
    }
    if (observed.f4seVersion != RuntimeVersion{0, 6, 23, 0})
    {
        return {false, RuntimeValidationReason::F4seVersionMismatch};
    }
    return {true, RuntimeValidationReason::Valid};
}

}  // namespace fo4hdr
