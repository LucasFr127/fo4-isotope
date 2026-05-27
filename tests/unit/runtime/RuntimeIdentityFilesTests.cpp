// SPDX-License-Identifier: GPL-3.0-or-later

#include "RuntimeIdentityFiles.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <Windows.h>
#include <winver.h>

#include <span>

namespace fo4hdr::tests {

void RunRuntimeIdentityFilesTests()
{
    VS_FIXEDFILEINFO info{};
    info.dwSignature = VS_FFI_SIGNATURE;
    info.dwStrucVersion = VS_FFI_STRUCVERSION;
    info.dwFileVersionMS = MAKELONG(10, 1);
    info.dwFileVersionLS = MAKELONG(0, 163);
    const auto bytes = std::as_bytes(std::span{&info, 1});
    const RuntimeVersion expected{1, 10, 163, 0};
    FO4HDR_CHECK(DecodeFixedFileVersion(bytes) == expected);
    FO4HDR_CHECK(!DecodeFixedFileVersion({}));
    FO4HDR_CHECK(!DecodeFixedFileVersion(bytes.first(bytes.size() - 1)));

    info.dwSignature = 0;
    FO4HDR_CHECK(!DecodeFixedFileVersion(bytes));
    info.dwSignature = VS_FFI_SIGNATURE;
    info.dwStrucVersion = 0;
    FO4HDR_CHECK(!DecodeFixedFileVersion(bytes));
    info.dwStrucVersion = VS_FFI_STRUCVERSION;
    info.dwFileVersionMS = MAKELONG(65535, 65535);
    info.dwFileVersionLS = MAKELONG(65535, 65535);
    const RuntimeVersion maximum{65535, 65535, 65535, 65535};
    FO4HDR_CHECK(DecodeFixedFileVersion(bytes) == maximum);

    const RuntimeVersion executableVersion{1, 10, 163, FO4HDR_TEST_EXECUTABLE_BUILD};
    FO4HDR_CHECK(ReadCurrentExecutableVersion() == executableVersion);
}

}  // namespace fo4hdr::tests
