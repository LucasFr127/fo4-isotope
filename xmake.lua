-- SPDX-License-Identifier: GPL-3.0-or-later

set_project("Fallout4HDR")
set_license("GPL-3.0-or-later")
set_languages("c++23")
set_toolchains("msvc")
set_allowedplats("windows")
set_allowedarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")
set_defaultplat("windows")
set_defaultarchs("windows|x64")
set_defaultmode("debug")
set_warnings("allextra")
set_encodings("utf-8")
add_rules("mode.debug", "mode.releasedbg")

if is_mode("debug") then
    set_runtimes("MTd")
else
    set_runtimes("MT")
end

target("fo4hdr-tests")
    set_kind("binary")
    set_targetdir("build/$(plat)/$(arch)/$(mode)")
    add_includedirs("include", "tests/support", "tests/unit")
    add_files(
        "src/output/Hdr10Color.cpp",
        "src/runtime/RuntimeValidation.cpp",
        "tests/unit/main.cpp",
        "tests/unit/output/HdrCoreTests.cpp",
        "tests/unit/runtime/RuntimeValidationTests.cpp")
target_end()
