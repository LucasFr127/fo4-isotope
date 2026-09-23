// SPDX-License-Identifier: GPL-3.0-or-later
#include "RuntimeIdentityFiles.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fo4hdr::tests {
namespace {
class HashFixture
{
public:
    HashFixture()
    {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("Fallout4HDR-hash-" + std::to_string(token));
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Hash fixture collision");
    }
    ~HashFixture() noexcept { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path path;
};

void WriteHashInput(const std::filesystem::path& path, const char* text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    if (!file) throw std::runtime_error("Hash fixture write failed");
}
} // namespace

void RunRuntimeIdentityFilesTests()
{
    HashFixture fixture;
    const auto crlf = fixture.path / "crlf.txt";
    const auto lf = fixture.path / "lf.txt";
    WriteHashInput(crlf, "authored\r\n");
    WriteHashInput(lf, "authored\n");
    FO4HDR_CHECK(!Sha256File(fixture.path / "absent.txt"));
    const auto raw = Sha256File(crlf);
    const auto canonical = Sha256CanonicalTextFile(crlf);
    FO4HDR_CHECK(raw && raw->size() == 64);
    FO4HDR_CHECK(canonical && canonical->size() == 64);
    FO4HDR_CHECK(raw != Sha256File(lf));
    FO4HDR_CHECK(canonical == Sha256CanonicalTextFile(lf));
}
} // namespace fo4hdr::tests
