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
set_targetdir("build/$(plat)/$(arch)/$(mode)")
add_rules("mode.debug", "mode.releasedbg")

if is_mode("debug") then
    set_runtimes("MTd")
else
    set_runtimes("MT")
end

local spdlog_source = os.getenv("FO4HDR_SPDLOG_SOURCE") or "lib/spdlog"
local commonlib_source = "lib/commonlibf4"
local shared_source = path.join(commonlib_source, "lib/commonlib-shared")

target("spdlog")
    set_kind("static")
    add_includedirs(path.join(spdlog_source, "include"), { public = true })
    -- CommonLib file sinks receive std::string paths.
    add_defines("SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT",
        "SPDLOG_WCHAR_TO_UTF8_SUPPORT", { public = true })
    add_files(path.join(spdlog_source, "src/spdlog.cpp"),
        path.join(spdlog_source, "src/stdout_sinks.cpp"),
        path.join(spdlog_source, "src/color_sinks.cpp"),
        path.join(spdlog_source, "src/file_sinks.cpp"),
        path.join(spdlog_source, "src/async.cpp"),
        path.join(spdlog_source, "src/cfg.cpp"))
target_end()

target("commonlib-shared")
    set_kind("static")
    add_deps("spdlog", { public = true })
    add_includedirs(path.join(shared_source, "include"), { public = true })
    add_defines("COMMONLIB_RUNTIMECOUNT=3", { public = true })
    add_files(path.join(shared_source, "src/**.cpp"))
    set_pcxxheader(path.join(shared_source, "src/REX/PCH.h"))
    add_cxxflags("/EHsc", "/permissive-", "cl::/bigobj", "cl::/cgthreads8",
        "cl::/diagnostics:caret", "cl::/external:W0", "cl::/fp:contract",
        "cl::/fp:except-", "cl::/guard:cf-", "cl::/Zc:enumTypes",
        "cl::/Zc:preprocessor", "cl::/Zc:templateScope", "cl::/wd4200",
        "cl::/wd4201", "cl::/wd4324", "cl::/we4715", { public = true })
    add_syslinks("advapi32", "bcrypt", "d3d11", "d3dcompiler", "dbghelp", "dxgi",
        "ole32", "shell32", "user32", "version", "ws2_32", { public = true })
target_end()

target("commonlibf4")
    set_kind("static")
    add_deps("commonlib-shared", { public = true })
    add_includedirs(path.join(commonlib_source, "include"), { public = true })
    add_defines("COMMONLIB_RUNTIMECOUNT=3", { public = true })
    add_files(path.join(commonlib_source, "src/**.cpp"))
    set_pcxxheader(path.join(commonlib_source, "include/F4SE/Impl/PCH.h"))
target_end()

target("Fallout4HDR")
    set_kind("shared")
    add_deps("commonlibf4")
    add_includedirs("include", "src/runtime")
    add_defines("COMMONLIB_RUNTIMECOUNT=3")
    add_files("src/plugin/PluginMain.cpp", "src/runtime/RuntimeIdentityFiles.cpp",
        "src/runtime/RuntimeValidation.cpp", "src/game/GameStateObserver.cpp")
    add_syslinks("version")
target_end()

for _, fixture in ipairs({
    { name = "fo4hdr-tests", build = 0 },
    { name = "fo4hdr-tests-executable-mismatch", build = 1 }
}) do
    target(fixture.name)
        set_kind("binary")
        add_rules("win.sdk.resource")
        add_defines("FO4HDR_TEST_EXECUTABLE_BUILD=" .. fixture.build)
        add_includedirs("include", "src/runtime", "src/game", "tests/support", "tests/unit")
        add_syslinks("version")
        add_files("src/config/HdrSettings.cpp", "src/output/Hdr10Color.cpp",
            "src/runtime/RuntimeValidation.cpp",
            "src/runtime/RuntimeIdentityFiles.cpp", "tests/unit/main.cpp",
            "tests/unit/output/HdrCoreTests.cpp", "tests/unit/runtime/RuntimeValidationTests.cpp",
            "tests/unit/game/GameSnapshotStoreTests.cpp",
            "tests/unit/config/HdrSettingsMathTests.cpp",
            "tests/unit/game/MenuEventQueueTests.cpp",
            "tests/unit/runtime/RuntimeIdentityFilesTests.cpp",
            "tests/unit/plugin/PluginBootstrapTests.cpp",
            "tests/unit/runtime/RuntimeVersionFixture.rc")
    target_end()
end

target("fo4hdr-shader-tests")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "tests/support")
    add_syslinks("d3d11")
    add_files("src/output/Hdr10Color.cpp", "tests/shader/main.cpp",
        "tests/support/d3d11/GpuHarness.cpp")
    before_build(function(target)
        local pwsh = os.getenv("FO4HDR_PWSH") or "pwsh"
        os.vrunv(pwsh, { "-NoProfile", "-File",
            path.join(os.projectdir(), "scripts/compile-shaders.ps1"),
            "-Output", path.join(os.projectdir(), target:targetdir(), "TransferEvaluation.dxbc") })
    end)
target_end()
