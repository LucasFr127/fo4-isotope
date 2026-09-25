-- SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

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

set_xmakever("3.0.0")
set_policy("package.requires_lock", true)
set_policy("package.precompiled", false)
includes("lib/commonlibf4")

set_project("Fallout4HDR")
set_version("0.0.0")
set_license("LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions")
set_languages("c++23")
add_requires("imgui v1.92.6-docking", {
    configs = { shared = false, dx11 = true, win32 = true,
        no_demo_windows = true, no_debug_tools = true }
})

option("fo4hdr_runtime")
    set_default("1.10.163")
    set_values("1.10.163", "1.10.984", "1.11.221")
option_end()
option("fo4hdr_allow_unvalidated_runtime")
    set_default(false)
option_end()
option("fo4hdr_use_pch")
    set_default(true)
option_end()
option("fo4hdr_build_tests")
    set_default(true)
option_end()

rule("fo4hdr.plugin")
    add_deps("commonlib.plugin")
    on_load(function(target)
        target:data_set("commonlib.plugin.config", target:extraconf("rules", "fo4hdr.plugin"))
    end)
rule_end()

local selected_runtime = get_config("fo4hdr_runtime") or "1.10.163"
if selected_runtime ~= "1.10.163" and not has_config("fo4hdr_allow_unvalidated_runtime") then
    os.raise("Planned runtime requires --fo4hdr_allow_unvalidated_runtime=y")
end
local commonlib_runtime_define = "COMMONLIB_RUNTIMECOUNT=3"

target("Fallout4HDR")
    add_rules("fo4hdr.plugin", { name = "Fallout4HDR", author = "Fallout4HDR contributors",
        description = "CommonLibF4 HDR output plugin for Fallout 4" })
    add_deps("commonlibf4", "fo4hdr-shaders")
    add_packages("imgui")
    if has_config("fo4hdr_use_pch") then
        set_pcxxheader("src/plugin/PluginPch.hpp")
    end
    add_files(path.join("runtime", selected_runtime, "PluginVersion.cpp"))
    add_includedirs("include", "src", "src/runtime", "src/graphics", "src/ui", "build/generated")
    add_defines(commonlib_runtime_define)
    if selected_runtime == "1.10.163" then
        add_defines("FO4HDR_RUNTIME_1_10_163=1")
    else
        add_defines("FO4HDR_RUNTIME_PLANNED=1")
    end
    add_files("src/config/HdrSettings.cpp",
        "src/config/PluginConfiguration.cpp",
        "src/config/SettingsPersistence.cpp",
        "src/game/ArmorUiObserver.cpp",
        "src/game/GameStateObserver.cpp",
        "src/game/ImageSpaceObserver.cpp",
        "src/game/ImageSpaceState.cpp",
        "src/game/NativeLutObserver.cpp",
        "src/game/RendererSnapshot.cpp",
        "src/graphics/d3d11/ContextQuerySafety.cpp",
        "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.cpp",
        "src/graphics/d3d11/ScopedD3D11PixelBindings.cpp",
        "src/graphics/DisplayDiscovery.cpp",
        "src/graphics/HdrRenderer.cpp",
        "src/graphics/hooks/ContextHooks.cpp",
        "src/graphics/hooks/DeviceHooks.cpp",
        "src/graphics/hooks/SwapChainHooks.cpp",
        "src/graphics/OperatorLutWorker.cpp",
        "src/graphics/passes/ArmorCompositionPass.cpp",
        "src/graphics/passes/ArmorEncodedComposition.cpp",
        "src/graphics/passes/NativeReferenceGradePass.cpp",
        "src/graphics/passes/ObservedDraw.cpp",
        "src/graphics/passes/PresentationPass.cpp",
        "src/graphics/resources/DeveloperExposureResources.cpp",
        "src/graphics/resources/NativeReferenceResources.cpp",
        "src/hooks/HookOwnership.cpp",
        "src/hooks/PeIat.cpp",
        "src/output/ArmorUiComposition.cpp",
        "src/output/Hdr10Color.cpp",
        "src/output/NativeLutObservation.cpp",
        "src/output/PerformanceStatistics.cpp",
        "src/output/SelectiveColor.cpp",
        "src/plugin/PluginMain.cpp",
        "src/runtime/RuntimeIdentityFiles.cpp",
        "src/runtime/RuntimeValidation.cpp",
        "src/tonemap/Aces2.cpp",
        "src/tonemap/aces2/Aces2ColorAppearance.cpp",
        "src/tonemap/aces2/Aces2Tables.cpp",
        "src/tonemap/Aces2Lmt.cpp",
        "src/tonemap/Agx.cpp",
        "src/tonemap/Gt7.cpp",
        "src/tonemap/OperatorLut.cpp",
        "src/ui/DisplayCalibrationPanel.cpp",
        "src/ui/HdrSettingsInput.cpp",
        "src/ui/LutProfilesPanel.cpp",
        "src/ui/SettingsPanel.cpp",
        "src/ui/SettingsWindow.cpp",
        "src/hooks/GraphicsHookSupport.cpp",
        "src/graphics/GraphicsDiagnostics.cpp",
        "src/graphics/PresentFrame.cpp",
        "src/graphics/ShaderRegistry.cpp",
        "src/graphics/d3d11/BorrowedComputeState.cpp",
        "src/graphics/resources/PresentationResources.cpp",
        "src/graphics/resources/ReplacementShaders.cpp",
        "src/graphics/resources/SceneResources.cpp",
        "src/graphics/resources/ArmorCompositionResources.cpp",
        "src/output/HdrActivationPolicy.cpp",
        "src/output/RenderPipelineContract.cpp")
    add_syslinks("version", "bcrypt", "d3d11", "dxgi", "user32")
    on_install(function(target)
        local stage = path.join(os.projectdir(), "dist", selected_runtime, "Data/F4SE/Plugins")
        local manifestdir = path.join(stage, "Fallout4HDR")
        local manifest = path.join(os.projectdir(), "runtime", selected_runtime, "manifest.json")
        local profilepack = path.join(os.projectdir(), "config/Fallout4HDR/Default.ini")
        assert(target:symbolfile(), "Missing plugin symbol file path")
        local files = { target:targetfile(), target:symbolfile(),
            path.join(os.projectdir(), "config/Fallout4HDR.ini") }
        local allowed = { ["Fallout4HDR.dll"] = true, ["Fallout4HDR.pdb"] = true,
            ["Fallout4HDR.ini"] = true }
        for _, file in ipairs(table.join(files, { manifest, profilepack })) do
            assert(os.isfile(file), "Missing staging input: " .. file)
        end
        for _, file in ipairs(os.files(path.join(stage, "*"))) do
            assert(allowed[path.filename(file)], "Unexpected staging file: " .. file)
        end
        for _, directory in ipairs(os.dirs(path.join(stage, "*"))) do
            assert(directory == manifestdir, "Unexpected staging directory: " .. directory)
        end
        local manifestname = "runtime-" .. selected_runtime .. ".json"
        local childfiles = { [manifestname] = true, ["Default.ini"] = true }
        for _, file in ipairs(os.files(path.join(manifestdir, "*"))) do
            assert(childfiles[path.filename(file)], "Unexpected plugin data staging file: " .. file)
        end
        assert(#os.dirs(path.join(manifestdir, "*")) == 0, "Unexpected manifest staging directory")
        os.mkdir(manifestdir)
        for _, file in ipairs(files) do
            os.cp(file, path.join(stage, path.filename(file)))
        end
        os.cp(manifest, path.join(manifestdir, manifestname))
        os.cp(profilepack, path.join(manifestdir, "Default.ini"))
    end)
target_end()

if has_config("fo4hdr_build_tests") then
    target("fo4hdr-tests")
        set_kind("binary")
        add_packages("imgui")
        add_includedirs("include", "src", "src/config", "src/game", "src/graphics", "src/ui", "src/runtime", "tests/support", "tests/unit")
        add_syslinks("version", "bcrypt", "d3d11", "user32")
        add_files("src/config/HdrSettings.cpp",
            "src/config/PluginConfiguration.cpp",
            "src/config/SettingsPersistence.cpp",
            "src/output/Hdr10Color.cpp",
            "src/output/SelectiveColor.cpp",
            "src/output/PerformanceStatistics.cpp",
            "src/output/RenderPipelineContract.cpp",
            "src/output/HdrActivationPolicy.cpp",
            "src/output/NativeLutObservation.cpp",
            "src/hooks/PeIat.cpp",
            "src/hooks/HookOwnership.cpp",
            "src/hooks/GraphicsHookSupport.cpp",
            "src/runtime/RuntimeValidation.cpp",
            "src/runtime/RuntimeIdentityFiles.cpp",
            "src/tonemap/OperatorLut.cpp",
            "src/tonemap/Gt7.cpp",
            "src/tonemap/Agx.cpp",
            "src/tonemap/Aces2.cpp",
            "src/tonemap/Aces2Lmt.cpp",
            "src/tonemap/aces2/Aces2ColorAppearance.cpp",
            "src/tonemap/aces2/Aces2Tables.cpp",
            "src/game/ImageSpaceState.cpp",
            "src/graphics/OperatorLutWorker.cpp",
            "src/graphics/d3d11/ContextQuerySafety.cpp",
            "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.cpp",
            "src/ui/DisplayCalibrationPanel.cpp",
            "tests/shader/ShaderTestOptions.cpp",
            "tests/support/reference/OperatorLutTestSupport.cpp",
            "tests/support/reference/Aces2LmtSourceOracle.cpp",
            "tests/support/reference/SelectiveColorSourceOracle.cpp",
            "tests/unit/config/HdrConfigurationTests.cpp",
            "tests/unit/config/HdrGradeProfileTests.cpp",
            "tests/unit/config/HdrPersistenceTests.cpp",
            "tests/unit/config/HdrSelectiveColorTests.cpp",
            "tests/unit/config/HdrSettingsMathTests.cpp",
            "tests/unit/game/DirectCallPatchTests.cpp",
            "tests/unit/game/ImageSpaceProvenanceTests.cpp",
            "tests/unit/graphics/HdrOperatorWorkerTests.cpp",
            "tests/unit/graphics/PipboyScreenObservationTests.cpp",
            "tests/unit/hooks/HookTests.cpp",
            "tests/unit/main.cpp",
            "tests/unit/output/HdrCoreTests.cpp",
            "tests/unit/output/PerformanceTests.cpp",
            "tests/unit/output/PipelineContractTests.cpp",
            "tests/unit/runtime/RuntimeIdentityFilesTests.cpp",
            "tests/unit/runtime/RuntimeValidationTests.cpp",
            "tests/unit/ShaderTestOptionsTests.cpp",
            "tests/unit/tonemap/AgxTests.cpp",
            "tests/unit/tonemap/Gt7Tests.cpp",
            "tests/unit/tonemap/OperatorLutTests.cpp",
            "tests/unit/tonemap/ToneMappingTests.cpp",
            "tests/unit/game/GameStateObserverTests.cpp",
            "tests/unit/TestHarnessTests.cpp",
            "tests/unit/output/ArmorUiCompositionTests.cpp",
            "tests/unit/output/NativeLutObservationTests.cpp",
            "tests/unit/graphics/DeveloperExposureLockTests.cpp",
            "tests/unit/ui/DisplayCalibrationTests.cpp",
            "tests/unit/config/HdrGradePackTests.cpp",
            "tests/unit/graphics/ScopedD3D11ComputeConstantBufferTests.cpp")
    target_end()
end

target("fo4hdr-shaders")
    set_kind("phony")
    set_default(false)
    -- Consumers need the generated headers before their compilation starts.
    set_policy("build.fence", true)

    on_build(function (target)
        local depend = import("core.project.depend")
        local json = import("core.base.json")

        local pwsh = os.getenv("FO4HDR_PWSH") or "pwsh"
        if os.isdir(pwsh) then
            pwsh = path.join(pwsh, "pwsh.exe")
        end
        local script = path.join(
            os.projectdir(), "scripts", "compile-shaders.ps1")
        local outputdir = path.join(
            os.projectdir(), "build", "generated")
        local dependency_json = os.iorunv(pwsh, {
            "-NoProfile",
            "-File",
            script,
            "-OutputDirectory",
            outputdir,
            "-PrintDependencyInfo"
        })
        local dependency = json.decode(dependency_json)
        local files = {
            script,
            path.join(os.projectdir(), "versions.lock.json"),
            dependency.compilerPath
        }
        table.join2(files, os.files(path.join(
            os.projectdir(), "scripts", "internal", "*.ps1")))
        table.join2(
            files,
            os.files(path.join(os.projectdir(), "shaders", "**.hlsl")))
        table.join2(
            files,
            os.files(path.join(os.projectdir(), "shaders", "**.hlsli")))
        table.join2(
            files,
            os.files(path.join(
                os.projectdir(), "tests", "shader", "**.hlsl")))
        table.join2(
            files,
            os.files(path.join(
                os.projectdir(), "tests", "shader", "**.hlsli")))
        -- Treat outputs as dependencies so deleting any artifact regenerates it.
        table.join2(files, dependency.outputs)
        table.sort(files)
        local compiler_identity =
            dependency.compilerPath .. "|" .. dependency.compilerVersion
        depend.on_changed(function ()
            os.execv(pwsh, {
                "-NoProfile",
                "-File",
                script,
                "-OutputDirectory",
                outputdir,
                "-FxcPath",
                dependency.compilerPath
            })
        end, {
            dependfile = path.join(target:dependir(), "shaders.d"),
            files = files,
            values = { compiler_identity }
        })
    end)
target_end()

if has_config("fo4hdr_build_tests") then
target("fo4hdr-shader-tests")
    set_kind("binary")
    set_default(false)
    add_deps("fo4hdr-shaders")
    add_packages("imgui")
    add_includedirs("include", "src", "src/graphics", "src/ui", "tests/support", "tests/unit", "src/config", "build/generated")
    add_syslinks("d3d11", "dxgi", "user32", "bcrypt")
    add_files("src/config/HdrSettings.cpp",
        "src/config/PluginConfiguration.cpp",
        "src/config/SettingsPersistence.cpp",
        "src/output/Hdr10Color.cpp",
        "src/output/SelectiveColor.cpp",
        "src/output/PerformanceStatistics.cpp",
        "src/output/RenderPipelineContract.cpp",
        "src/output/HdrActivationPolicy.cpp",
        "src/output/NativeLutObservation.cpp",
        "src/hooks/PeIat.cpp",
        "src/hooks/HookOwnership.cpp",
        "src/hooks/GraphicsHookSupport.cpp",
        "src/runtime/RuntimeValidation.cpp",
        "src/runtime/RuntimeIdentityFiles.cpp",
        "src/tonemap/OperatorLut.cpp",
        "src/tonemap/Gt7.cpp",
        "src/tonemap/Agx.cpp",
        "src/tonemap/Aces2.cpp",
        "src/tonemap/Aces2Lmt.cpp",
        "src/tonemap/aces2/Aces2ColorAppearance.cpp",
        "src/tonemap/aces2/Aces2Tables.cpp",
        "src/graphics/d3d11/ContextQuerySafety.cpp",
        "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.cpp",
        "src/graphics/d3d11/ScopedD3D11PixelBindings.cpp",
        "src/graphics/d3d11/BorrowedComputeState.cpp",
        "src/graphics/passes/ArmorEncodedComposition.cpp",
        "src/graphics/passes/NativeReferenceGradePass.cpp",
        "src/graphics/resources/NativeReferenceResources.cpp",
        "src/ui/SettingsPanel.cpp",
        "src/ui/LutProfilesPanel.cpp",
        "src/ui/DisplayCalibrationPanel.cpp",
        "tests/unit/ui/LutProfilesPanelTests.cpp",
        "tests/unit/tonemap/Gt7Tests.cpp",
        "tests/shader/Gt7ReferenceTests.cpp",
        "tests/shader/HdrGradeTransferTests.cpp",
        "tests/shader/main.cpp",
        "tests/shader/OperatorLutTests.cpp",
        "tests/shader/SettingsPanelTests.cpp",
        "tests/shader/ShaderTestOptions.cpp",
        "tests/shader/ArmorCompositionTests.cpp",
        "tests/shader/CreativeGradeLutTests.cpp",
        "tests/shader/DeveloperExposureLockTests.cpp",
        "tests/shader/Gt7OperatorUiTests.cpp",
        "tests/shader/ImageControlsTests.cpp",
        "tests/shader/NativeReferenceGradeTests.cpp",
        "tests/shader/PresentationPassTests.cpp",
        "tests/shader/SeparatedUiTests.cpp",
        "tests/shader/ShippingPixelShaderTests.cpp",
        "tests/shader/StateRestorationTests.cpp",
        "tests/support/d3d11/GpuHarness.cpp",
        "tests/support/reference/OperatorLutTestSupport.cpp",
        "tests/support/reference/ImageControlReference.cpp",
        "tests/support/reference/NativeGradeReference.cpp",
        "tests/support/reference/ShaderReference.cpp")
target_end()
    target("fo4hdr-header-checks")
        set_kind("static")
        set_default(true)
        add_deps("fo4hdr-shaders", "commonlibf4")
        add_packages("imgui")
        add_defines(commonlib_runtime_define)
        add_includedirs("include", ".", "src", "src/config", "src/game", "src/graphics", "src/ui",
            "src/runtime", "tests/support",
            path.join(os.projectdir(), "build", "generated"))
        if (get_config("fo4hdr_runtime") or "1.10.163") == "1.10.163" then
            add_defines("FO4HDR_RUNTIME_1_10_163=1")
        end
        on_load(function (target)
            local headers = os.files("include/**.hpp")
            table.join2(headers, os.files("src/**.hpp"))
            table.join2(headers, os.files("tests/unit/**.hpp"))
            table.join2(headers, os.files("tests/shader/**.hpp"))
            table.join2(headers, os.files("tests/support/**.hpp"))
            table.sort(headers)
            local outputdir = path.join(os.projectdir(), "build", "header-checks")
            os.mkdir(outputdir)
            for _, header in ipairs(headers) do
                local include = header:gsub("\\", "/")
                local output = path.join(outputdir, include:gsub("/", "_") .. ".cpp")
                local content = '#include "' .. include .. '"\n'
                if not os.isfile(output) or io.readfile(output) ~= content then
                    io.writefile(output, content)
                end
                target:add("files", output)
            end
        end)
    target_end()
end
