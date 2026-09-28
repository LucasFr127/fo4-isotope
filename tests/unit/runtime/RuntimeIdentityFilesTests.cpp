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
    FO4HDR_CHECK(!Sha256CanonicalTextFile(missingPath).has_value());
    FO4HDR_CHECK(!Sha256File(root).has_value());
    FO4HDR_CHECK(!Sha256CanonicalTextFile(root).has_value());

    const auto lfPath = root / "lf.txt";
    const auto crlfPath = root / "crlf.txt";
    FO4HDR_CHECK(write(lfPath, "alpha\nbeta\n"));
    FO4HDR_CHECK(write(crlfPath, "alpha\r\nbeta\r\n"));
    const auto lfHash = Sha256CanonicalTextFile(lfPath);
    const auto crlfHash = Sha256CanonicalTextFile(crlfPath);
    FO4HDR_CHECK(lfHash.has_value());
    FO4HDR_CHECK(crlfHash.has_value());
    FO4HDR_CHECK(
        lfHash.value_or("") ==
        "e49c81e2d2f84e259d40e2fb8192f3bc"
        "d198b355184845d76d8f58807d0d78ee");
    FO4HDR_CHECK(lfHash == crlfHash);

    const auto bomPath = root / "bom.txt";
    const auto loneCrPath = root / "lone-cr.txt";
    const auto noFinalLfPath = root / "no-final-lf.txt";
    const auto emptyPath = root / "empty.txt";
    FO4HDR_CHECK(write(bomPath, "\xEF\xBB\xBF" "alpha\n"));
    FO4HDR_CHECK(write(loneCrPath, "alpha\rbeta\n"));
    FO4HDR_CHECK(write(noFinalLfPath, "alpha\nbeta"));
    FO4HDR_CHECK(write(emptyPath, ""));
    FO4HDR_CHECK(!Sha256CanonicalTextFile(bomPath).has_value());
    FO4HDR_CHECK(!Sha256CanonicalTextFile(loneCrPath).has_value());
    FO4HDR_CHECK(!Sha256CanonicalTextFile(noFinalLfPath).has_value());
    FO4HDR_CHECK(!Sha256CanonicalTextFile(emptyPath).has_value());

    std::filesystem::remove_all(root, error);
    FO4HDR_CHECK(!error);
}

}  // namespace fo4hdr::tests
