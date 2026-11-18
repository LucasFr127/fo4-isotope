// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstddef>
#include <filesystem>
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

enum class PeExportStatus
{
    found,
    notFound,
    malformed
};

// Looks up an exact export name in an image laid out by RVA. An image for
// another machine type never exports it.
[[nodiscard]] PeExportStatus FindPe32PlusExport(
    std::span<const std::byte> image, std::string_view symbol) noexcept;

enum class PluginExportScan
{
    found,
    absent,
    unknown
};

// Whether a DLL directly inside directory exports symbol, read without running
// it. A file or directory that cannot be read makes an otherwise absent result
// unknown.
[[nodiscard]] PluginExportScan ScanPluginExports(
    const std::filesystem::path& directory, std::string_view symbol) noexcept;

}  // namespace fo4hdr
