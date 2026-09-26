-- SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

set_xmakever("3.0.0")
set_policy("package.requires_lock", true)
set_policy("package.precompiled", false)
set_toolchains("msvc")

set_allowedplats("windows")
set_allowedarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")
set_defaultplat("windows")
set_defaultarchs("windows|x64")
set_defaultmode("debug")

if is_mode("debug") then
    set_runtimes("MTd")
else
    set_runtimes("MT")
end

if not os.isdir("lib/commonlibf4") then
    os.raise(
        "Missing lib/commonlibf4. Run git submodule update --init --recursive.")
end

-- CommonLib is included before defining Fallout4HDR targets so its shared
-- targets and rules are available. The custom plugin rule below deliberately
-- uses the lower-level commonlib.plugin rule because Fallout4HDR supplies a
-- runtime-specific PluginVersion.cpp.
includes("lib/commonlibf4")

add_requires("imgui v1.92.6-docking", {
    configs = {
        shared = false,
        dx11 = true,
        win32 = true,
        no_demo_windows = true,
        no_debug_tools = true
    }
})

set_project("Fallout4HDR")
set_version("0.0.0")
set_license("LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
add_defines("NOMINMAX")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

option("fo4hdr_runtime")
    set_default("1.10.163")
    set_values("1.10.163", "1.10.984", "1.11.221")
    set_showmenu(true)
    set_description("Fallout 4 runtime package")
option_end()

option("fo4hdr_allow_unvalidated_runtime")
    set_default(false)
    set_showmenu(true)
    set_description("Allow developer builds for planned runtime ports")
option_end()

option("fo4hdr_build_tests")
    set_default(true)
    set_showmenu(true)
option_end()

option("fo4hdr_use_pch")
    set_default(true)
    set_showmenu(true)
    set_description("Use the plugin-only precompiled header")
option_end()

rule("fo4hdr.plugin")
    add_deps("commonlib.plugin")

    on_load(function (target)
        target:data_set(
            "commonlib.plugin.config",
            target:extraconf("rules", "fo4hdr.plugin"))
        target:data_set(
            "commonlib.plugin.package",
            { prefixdir = "Data" })
    end)

    on_config(function (target)
        target:add("deps", "commonlibf4")

        local stage = path.join(os.projectdir(), "dist")
        target:set(
            "installdir",
            path.join(
                stage,
                get_config("fo4hdr_runtime"),
                "Data"))

        target:add(
            "installfiles",
            target:targetfile(),
            { prefixdir = "F4SE/Plugins" })
        target:add(
            "installfiles",
            target:symbolfile(),
            { prefixdir = "F4SE/Plugins" })
        target:add(
            "installfiles",
            "config/Fallout4HDR.ini",
            { prefixdir = "F4SE/Plugins" })
        target:add(
            "installfiles",
            "config/Fallout4HDR/Default.ini",
            { prefixdir = "F4SE/Plugins/Fallout4HDR" })
        target:add(
            "installfiles",
            path.join(
                "runtime",
                get_config("fo4hdr_runtime"),
                "manifest.json"),
            {
                prefixdir = "F4SE/Plugins/Fallout4HDR",
                filename =
                    "runtime-" ..
                    get_config("fo4hdr_runtime") ..
                    ".json"
            })
    end)
rule_end()

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

target("fo4hdr-core")
    set_kind("static")
    set_default(true)
    add_files("src/tonemap/**.cpp")
    add_files("src/runtime/**.cpp")
    add_files(
        "src/hooks/GraphicsHookSupport.cpp",
        "src/hooks/HookOwnership.cpp",
        "src/hooks/PeIat.cpp")
    add_files(
        "src/config/HdrSettings.cpp",
        "src/config/PluginConfiguration.cpp",
        "src/config/SettingsPersistence.cpp")
    add_files(
        "src/output/RenderPipelineContract.cpp",
        "src/output/NativeLutObservation.cpp",
        "src/output/ArmorUiComposition.cpp")
    add_files(
        "src/output/Hdr10Color.cpp",
        "src/output/HdrActivationPolicy.cpp",
        "src/output/SelectiveColor.cpp",
        "src/output/PerformanceStatistics.cpp")
    add_headerfiles("include/Fallout4HDR/**.hpp")
    add_includedirs("include", { public = true })
target_end()

target("fo4hdr-renderer")
    set_kind("static")
    set_default(true)
    add_deps("fo4hdr-core", "fo4hdr-shaders")
    add_files("src/graphics/**.cpp")
    add_files("src/ui/**.cpp")
    add_headerfiles("include/Fallout4HDR/**.hpp")
    add_includedirs("include", { public = true })
    add_includedirs(path.join(os.projectdir(), "build", "generated"))
    add_packages("imgui")
    add_syslinks("d3d11", "dxgi", "dxguid", "user32", { public = true })

    if (get_config("fo4hdr_runtime") or "1.10.163") == "1.10.163" then
        add_defines("FO4HDR_RUNTIME_1_10_163=1")
    end

target_end()

target("Fallout4HDR")
    add_rules("fo4hdr.plugin", {
        name = "Fallout4HDR",
        author = "Fallout4HDR contributors",
        description =
            "CommonLibF4 HDR output plugin for Fallout 4"
    })
    add_deps("fo4hdr-core", "fo4hdr-renderer")
    add_files("src/plugin/**.cpp")
    add_includedirs("include")
    add_syslinks("bcrypt", "d3d11", "dxgi", "dxguid", "user32", "gdi32")
    if has_config("fo4hdr_use_pch") then
        set_pcxxheader("src/plugin/PluginPch.hpp")
    end

    local runtime = get_config("fo4hdr_runtime") or "1.10.163"
    if runtime == "1.10.163" then
        add_files("src/game/**.cpp")
        add_files("runtime/1.10.163/PluginVersion.cpp")
        add_defines("FO4HDR_RUNTIME_1_10_163=1")
    elseif runtime == "1.10.984" then
        if not has_config("fo4hdr_allow_unvalidated_runtime") then
            os.raise(
                "1.10.984 is not validated. Reconfigure explicitly with " ..
                "--fo4hdr_allow_unvalidated_runtime=y for porting work.")
        end
        add_files("runtime/1.10.984/PluginVersion.cpp")
        add_defines("FO4HDR_RUNTIME_1_10_984=1")
    elseif runtime == "1.11.221" then
        if not has_config("fo4hdr_allow_unvalidated_runtime") then
            os.raise(
                "1.11.221 is not validated. Reconfigure explicitly with " ..
                "--fo4hdr_allow_unvalidated_runtime=y for porting work.")
        end
        add_files("runtime/1.11.221/PluginVersion.cpp")
        add_defines("FO4HDR_RUNTIME_1_11_221=1")
    else
        os.raise(
            "Unsupported fo4hdr_runtime: " ..
            tostring(runtime))
    end
target_end()

if has_config("fo4hdr_build_tests") then
    target("fo4hdr-tests")
        set_kind("binary")
        set_rundir("$(projectdir)")
        set_default(true)
        add_deps("fo4hdr-core")
        add_files("tests/unit/**.cpp")
        add_files("tests/support/reference/OperatorLutTestSupport.cpp",
            "tests/support/reference/Aces2LmtSourceOracle.cpp",
            "tests/support/reference/SelectiveColorSourceOracle.cpp")
        add_files("tests/shader/ShaderTestOptions.cpp")
        add_files("src/game/ImageSpaceState.cpp")
        add_syslinks("bcrypt", "d3d11", "user32")
        add_packages("imgui")
        add_files("src/ui/HdrSettingsInput.cpp")
        add_files("src/ui/DisplayCalibrationPanel.cpp",
            "src/ui/ToneMapperPanel.cpp", "src/ui/LutProfilesPanel.cpp")
        add_files("src/graphics/OperatorLutWorker.cpp")
        add_files("src/graphics/d3d11/ContextQuerySafety.cpp",
            "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.cpp")
        add_includedirs("include", "src/config", "src/game", "src/graphics", "src/ui",
            "src/runtime", "tests/support")
    target_end()

    target("fo4hdr-shader-tests")
        set_kind("binary")
        set_default(true)
        add_deps("fo4hdr-core", "fo4hdr-shaders")
        add_files("tests/shader/**.cpp")
        add_files("tests/support/**.cpp")
        remove_files("tests/support/reference/Aces2LmtSourceOracle.cpp",
            "tests/support/reference/SelectiveColorSourceOracle.cpp")
        add_files("src/graphics/d3d11/**.cpp")
        add_files("src/graphics/passes/ArmorEncodedComposition.cpp",
            "src/graphics/passes/NativeReferenceGradePass.cpp",
            "src/graphics/resources/NativeReferenceResources.cpp")
        add_includedirs(
            "include", "src/graphics", "tests/support",
            path.join(os.projectdir(), "build", "generated"))
        add_syslinks("d3d11", "dxgi", "user32")

    target_end()

    target("fo4hdr-header-checks")
        set_kind("static")
        set_default(true)
        add_deps("fo4hdr-core", "fo4hdr-shaders", "commonlibf4")
        add_packages("imgui")
        add_includedirs("include", ".", "src/config", "src/game", "src/graphics", "src/ui",
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
