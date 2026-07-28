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

local dependency_profile = {
    spdlog_source = os.getenv("FO4HDR_SPDLOG_SOURCE") or "lib/spdlog",
    imgui_source = os.getenv("FO4HDR_IMGUI_SOURCE") or "lib/imgui",
    commonlib_source = "lib/commonlibf4",
    runtime_count = 3
}
dependency_profile.shared_source = path.join(dependency_profile.commonlib_source, "lib/commonlib-shared")
local commonlib_runtime_define = "COMMONLIB_RUNTIMECOUNT=" .. dependency_profile.runtime_count

target("spdlog")
    set_kind("static")
    add_includedirs(path.join(dependency_profile.spdlog_source, "include"), { public = true })
    -- CommonLib file sinks receive std::string paths.
    add_defines("SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT",
        "SPDLOG_WCHAR_TO_UTF8_SUPPORT", { public = true })
    add_files(path.join(dependency_profile.spdlog_source, "src/spdlog.cpp"),
        path.join(dependency_profile.spdlog_source, "src/stdout_sinks.cpp"),
        path.join(dependency_profile.spdlog_source, "src/color_sinks.cpp"),
        path.join(dependency_profile.spdlog_source, "src/file_sinks.cpp"),
        path.join(dependency_profile.spdlog_source, "src/async.cpp"),
        path.join(dependency_profile.spdlog_source, "src/cfg.cpp"))
target_end()

target("imgui-core")
    set_kind("static")
    set_default(false)
    add_includedirs(dependency_profile.imgui_source, { public = true })
    add_files(path.join(dependency_profile.imgui_source, "imgui.cpp"),
        path.join(dependency_profile.imgui_source, "imgui_draw.cpp"),
        path.join(dependency_profile.imgui_source, "imgui_tables.cpp"),
        path.join(dependency_profile.imgui_source, "imgui_widgets.cpp"))
    add_syslinks("user32", "kernel32", "shell32", "imm32", { public = true })
target_end()

target("commonlib-shared")
    set_kind("static")
    add_deps("spdlog", { public = true })
    add_includedirs(path.join(dependency_profile.shared_source, "include"), { public = true })
    add_defines(commonlib_runtime_define, { public = true })
    add_files(path.join(dependency_profile.shared_source, "src/**.cpp"))
    set_pcxxheader(path.join(dependency_profile.shared_source, "src/REX/PCH.h"))
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
    add_includedirs(path.join(dependency_profile.commonlib_source, "include"), { public = true })
    add_defines(commonlib_runtime_define, { public = true })
    add_files(path.join(dependency_profile.commonlib_source, "src/**.cpp"))
    set_pcxxheader(path.join(dependency_profile.commonlib_source, "include/F4SE/Impl/PCH.h"))
target_end()

target("Fallout4HDR")
    set_kind("shared")
    add_deps("commonlibf4")
    add_includedirs("include", "src/runtime")
    add_defines(commonlib_runtime_define)
    add_files("src/plugin/PluginMain.cpp", "src/runtime/RuntimeIdentityFiles.cpp",
        "src/runtime/RuntimeValidation.cpp", "src/game/GameStateObserver.cpp",
        "src/config/PluginConfiguration.cpp", "src/config/HdrSettings.cpp")
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
        add_includedirs("include", "src/runtime", "src/game", "src/graphics", "tests/support", "tests/unit")
        add_syslinks("version")
        add_syslinks("d3d11", "user32")
        add_files("src/config/HdrSettings.cpp", "src/output/Hdr10Color.cpp",
            "src/config/PluginConfiguration.cpp", "src/config/SettingsPersistence.cpp",
            "tests/unit/config/HdrPersistenceTests.cpp",
            "tests/shader/ShaderTestOptions.cpp", "tests/unit/ShaderTestOptionsTests.cpp",
            "src/tonemap/Gt7.cpp", "tests/unit/tonemap/Gt7Tests.cpp",
            "src/tonemap/Agx.cpp", "src/tonemap/Aces2.cpp", "src/tonemap/aces2/Aces2ColorAppearance.cpp",
            "src/tonemap/aces2/Aces2Tables.cpp", "tests/unit/tonemap/ToneMappingTests.cpp",
            "src/tonemap/OperatorLut.cpp", "src/graphics/OperatorLutWorker.cpp",
            "tests/support/reference/OperatorLutTestSupport.cpp",
            "tests/unit/tonemap/OperatorLutTests.cpp", "tests/unit/graphics/HdrOperatorWorkerTests.cpp",
            "src/hooks/PeIat.cpp", "src/hooks/HookOwnership.cpp",
            "tests/unit/hooks/HookTests.cpp",
            "src/graphics/DeviceCreationHook.cpp",
            "src/graphics/SwapChainObservation.cpp",
            "src/graphics/RendererLifetime.cpp",
            "tests/unit/graphics/DeviceCreationHookTests.cpp",
            "tests/unit/graphics/SwapChainObservationTests.cpp",
            "tests/unit/graphics/PresentationCharacterizationTests.cpp",
            "src/runtime/RuntimeValidation.cpp",
            "src/runtime/RuntimeIdentityFiles.cpp", "tests/unit/main.cpp",
            "tests/unit/output/HdrCoreTests.cpp", "tests/unit/runtime/RuntimeValidationTests.cpp",
            "tests/unit/output/PipelineContractTests.cpp",
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
    add_deps("imgui-core")
    add_includedirs("include", "src/graphics", "src/ui", "tests/support", "tests/unit", "build/include")
    add_syslinks("d3d11", "dxgi", "user32")
    add_files("src/tonemap/Gt7.cpp", "tests/unit/tonemap/Gt7Tests.cpp",
        "tests/shader/Gt7ReferenceTests.cpp",
        "src/output/Hdr10Color.cpp", "src/graphics/HdrRenderer.cpp",
        "src/graphics/passes/PresentationPass.cpp",
        "src/graphics/RendererLifetime.cpp", "tests/shader/main.cpp",
        "tests/shader/ShaderTestOptions.cpp",
        "tests/shader/ShippingPixelShaderTests.cpp", "tests/shader/PresentationPassTests.cpp",
        "tests/support/d3d11/GpuHarness.cpp", "tests/shader/OperatorLutTests.cpp",
        "tests/support/reference/OperatorLutTestSupport.cpp",
        "src/graphics/d3d11/ScopedD3D11PixelBindings.cpp",
        "src/config/HdrSettings.cpp", "src/config/PluginConfiguration.cpp",
        "src/ui/SettingsPanel.cpp", "tests/shader/SettingsPanelTests.cpp",
        "src/graphics/resources/DeveloperExposureResources.cpp",
        "tests/shader/DeveloperExposureTests.cpp",
        "src/config/SettingsPersistence.cpp", "src/graphics/OperatorLutWorker.cpp",
        "src/tonemap/OperatorLut.cpp", "src/tonemap/Agx.cpp", "src/tonemap/Aces2.cpp",
        "src/tonemap/aces2/Aces2ColorAppearance.cpp", "src/tonemap/aces2/Aces2Tables.cpp")
    before_build(function(target)
        local pwsh = os.getenv("FO4HDR_PWSH") or "pwsh"
        os.vrunv(pwsh, { "-NoProfile", "-File",
            path.join(os.projectdir(), "scripts/compile-shaders.ps1"),
            "-Output", path.join(os.projectdir(), target:targetdir(), "TransferEvaluation.dxbc") })
    end)
target_end()
