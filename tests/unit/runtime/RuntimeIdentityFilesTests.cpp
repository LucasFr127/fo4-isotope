// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "TestHarness.hpp"

#include "RuntimeIdentityFiles.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fo4hdr::tests {

void RunRuntimeIdentityFilesTests()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("FO4Isotope-runtime-identity-files-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    std::error_code error;
    const auto created = std::filesystem::create_directory(root, error);
    FO4HDR_CHECK(created && !error);
    if (!created || error)
    {
        return;
    }

    const auto write = [](const std::filesystem::path& path,
                          std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return output.good();
    };

    const auto binaryPath = root / "known.bin";
    FO4HDR_CHECK(write(binaryPath, std::string_view{"a\0b", 3}));
    FO4HDR_CHECK(
        Sha256File(binaryPath).value_or("") ==
        "59b271ae1bbcb1d31d41929817f4b16f"
        "b439eb4f31520b5ad1d5ce98920a7138");

    const auto missingPath = root / "missing.bin";
    FO4HDR_CHECK(!Sha256File(missingPath).has_value());
    FO4HDR_CHECK(!Sha256File(root).has_value());

    std::filesystem::remove_all(root, error);
    FO4HDR_CHECK(!error);
}

}  // namespace fo4hdr::tests
