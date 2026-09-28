// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace fo4hdr {

enum class PeIatStatus
{
    success,
    invalidArgument,
    invalidDos,
    invalidNt,
    notPe32PlusAmd64,
    importsUnavailable,
    malformedImports,
    moduleNotFound,
    symbolNotFound,
    nullTarget,
    duplicateSymbol
};

struct PeIatResult
{
    PeIatStatus status{PeIatStatus::invalidArgument};
    void** slot{};
};

[[nodiscard]] PeIatResult FindPe32PlusIatSlot(
    std::span<std::byte> image,
    std::string_view importedModule,
    std::string_view importedSymbol) noexcept;

}  // namespace fo4hdr
