# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
. ([IO.Path]::Combine($PSScriptRoot, "../../scripts/internal/Project.Source.ps1"))

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, "../.."))
$script = [IO.Path]::Combine($root, "scripts/package-source.ps1")
$expected = [string[]]@(
    ".clang-format", ".editorconfig", ".gitattributes", ".gitignore", ".gitmodules",
    "COPYING.CommonLib-Exceptions", "COPYING.GPL-3.0-or-later",
    "THIRD_PARTY_NOTICES.md",
    "include/Fallout4HDR/config/HdrSettings.hpp",
    "include/Fallout4HDR/game/GameHdrSnapshot.hpp",
    "include/Fallout4HDR/graphics/DeviceCreationObservation.hpp",
    "include/Fallout4HDR/hooks/PeIat.hpp",
    "include/Fallout4HDR/hooks/HookOwnership.hpp",
    "include/Fallout4HDR/output/Hdr10Color.hpp",
    "include/Fallout4HDR/output/RenderPipelineContract.hpp",
    "include/Fallout4HDR/runtime/RuntimeValidation.hpp",
    "scripts/package-source.ps1", "src/game/GameSnapshotStore.hpp",
    "src/game/GameStateObserver.cpp", "src/game/GameStateObserver.hpp",
    "src/game/MenuEventQueue.hpp",
    "src/hooks/PeIat.cpp",
    "src/hooks/HookOwnership.cpp",
    "src/graphics/DeviceCreationHook.hpp",
    "src/graphics/DeviceCreationHook.cpp",
    "src/graphics/ScenePassRegistration.cpp",
    "src/graphics/d3d11/ContextQuerySafety.hpp",
    "src/graphics/d3d11/ContextQuerySafety.cpp",
    "src/graphics/hooks/ContextHooks.hpp",
    "src/graphics/hooks/ContextHooks.cpp",
    "src/graphics/hooks/DeviceHooks.hpp",
    "src/graphics/hooks/DeviceHooks.cpp",
    "src/graphics/hooks/SwapChainHooks.hpp",
    "src/graphics/hooks/SwapChainHooks.cpp",
    "src/graphics/hooks/HookRecords.hpp",
    "src/graphics/hooks/NativeImageSlots.hpp",
    "src/graphics/hooks/NativeImageSlots.cpp",
    "tests/unit/hooks/NativeImageSlotsTests.cpp",
    "tests/shader/NativeRoutingTests.cpp",
    "src/graphics/GameDeviceGeneration.hpp",
    "src/graphics/GameDeviceGeneration.cpp",
    "src/graphics/SwapChainObservation.hpp",
    "src/graphics/SwapChainObservation.cpp",
    "src/graphics/RendererLifetime.hpp",
    "src/graphics/RendererLifetime.cpp",
    "src/graphics/RenoDxShaderIdentity.hpp",
    "src/config/HdrSettings.cpp",
    "src/output/Hdr10Color.cpp", "src/plugin/PluginMain.cpp",
    "src/runtime/RuntimeIdentityFiles.cpp", "src/runtime/RuntimeIdentityFiles.hpp",
    "src/runtime/RuntimeValidation.cpp", "tests/source/PackageSource.Tests.ps1", "tests/source/SourceTree.Tests.ps1",
    "tests/support/TestHarness.hpp", "tests/unit/main.cpp", "tests/unit/output/HdrCoreTests.cpp",
    "tests/unit/output/PipelineContractTests.cpp",
    "tests/unit/config/HdrSettingsMathTests.cpp",
    "tests/unit/game/GameSnapshotStoreTests.cpp",
    "tests/unit/game/MenuEventQueueTests.cpp",
    "tests/unit/hooks/HookTests.cpp",
    "tests/unit/graphics/DeviceCreationHookTests.cpp",
    "tests/unit/graphics/SwapChainObservationTests.cpp",
    "tests/unit/plugin/PluginBootstrapTests.cpp", "tests/unit/runtime/RuntimeIdentityFilesTests.cpp",
    "tests/unit/runtime/RuntimeValidationTests.cpp", "tests/unit/runtime/RuntimeVersionFixture.rc",
    "tests/unit/UnitTestSuite.hpp", "xmake.lua",
    "tests/shader/main.cpp",
    "tests/shader/shaders/TransferEvaluation.cs.hlsl",
    "tests/support/d3d11/GpuHarness.hpp",
    "tests/support/d3d11/GpuHarness.cpp",
    "scripts/compile-shaders.ps1",
    "tests/source/SourceText.Tests.ps1",
    "tests/source/PluginBoundarySource.Tests.ps1",
    "tests/source/ArchitectureLayout.Tests.ps1",
    "scripts/audit-f4se-project.ps1",
    "shaders/fallout4/CreativeGradeLut.ps.hlsl",
    "shaders/fallout4/GradeTransfer.hlsli",
    "shaders/fallout4/ToneMapA.ps.hlsl",
    "shaders/fallout4/ToneMapB.ps.hlsl",
    "shaders/fallout4/ToneMapBypassCommon.hlsli",
    "shaders/fallout4/ToneMapNewA.ps.hlsl",
    "shaders/fallout4/ToneMapNewB.ps.hlsl",
    "src/graphics/HdrRenderer.hpp",
    "src/graphics/HdrRenderer.cpp",
    "src/graphics/resources/HdrResourcePolicy.hpp",
    "tests/shader/ShippingPixelShaderTests.hpp",
    "tests/shader/ShaderTestSuite.hpp",
    "tests/shader/ShippingPixelShaderTests.cpp",
    "shaders/armor/EncodedComposition.cs.hlsl",
    "src/graphics/passes/ArmorEncodedComposition.hpp",
    "src/graphics/passes/ArmorEncodedComposition.cpp",
    "tests/shader/ArmorCompositionTests.cpp",
    "tests/shader/shaders/ArmorEncodedSource.ps.hlsl",
    "tests/shader/shaders/Fullscreen.vs.hlsl",
    "tests/shader/shaders/ScenePassSentinel.ps.hlsl",
    "shaders/fallout4/TemporalAa.ps.hlsl",
    "shaders/fallout4/TemporalAa2.ps.hlsl",
    "shaders/fallout4/TemporalIntermediate.hlsli",
    "shaders/fallout4/Fxaa.ps.hlsl",
    "shaders/fallout4/Video.ps.hlsl",
    "src/graphics/passes/PresentationPass.hpp",
    "src/graphics/passes/ObservedDraw.hpp",
    "src/graphics/passes/ObservedDraw.cpp",
    "src/game/ArmorUiObserver.hpp",
    "src/game/ArmorUiObserver.cpp",
    "src/game/DirectCallPatch.hpp",
    "tests/unit/game/DirectCallPatchTests.cpp",
    "include/Fallout4HDR/output/ArmorUiComposition.hpp",
    "src/output/ArmorUiComposition.cpp",
    "src/graphics/passes/ArmorCompositionPass.hpp",
    "src/graphics/passes/ArmorCompositionPass.cpp",
    "src/graphics/passes/PresentationPass.cpp",
    "shaders/output/DisplayMath.hlsli",
    "shaders/output/SceneLayers.hlsli",
    "shaders/output/PresentPixels.cs.hlsl",
    "tests/shader/PresentationPassTests.cpp",
    "LICENSES/Apache-2.0.txt",
    "include/Fallout4HDR/tonemap/Aces2.hpp",
    "include/Fallout4HDR/tonemap/Aces2Lmt.hpp",
    "src/tonemap/Aces2.cpp",
    "src/tonemap/Aces2Lmt.cpp",
    "src/tonemap/aces2/Aces2ColorAppearance.cpp",
    "src/tonemap/aces2/Aces2Model.hpp",
    "src/tonemap/aces2/Aces2Tables.cpp",
    "tests/unit/tonemap/ToneMappingTests.cpp",
    "tests/unit/tonemap/AgxTests.cpp",
    "LICENSES/BSD-3-Clause.txt",
    "include/Fallout4HDR/tonemap/Agx.hpp",
    "src/tonemap/Agx.cpp",
    "include/Fallout4HDR/tonemap/Gt7.hpp",
    "src/tonemap/Gt7.cpp",
    "shaders/tonemap/Gt7.hlsli",
    "tests/unit/tonemap/Gt7Tests.cpp",
    "tests/shader/Gt7ReferenceTests.hpp",
    "tests/shader/Gt7ReferenceTests.cpp",
    "tests/shader/shaders/Gt7Evaluation.cs.hlsl",
    "third_party/gt7/upstream/gt7_tone_mapping.cpp",
    "third_party/gt7/UPSTREAM.lock.json",
    "scripts/fetch-gt7-reference.ps1",
    "include/Fallout4HDR/tonemap/OperatorLut.hpp",
    "src/tonemap/OperatorLut.cpp",
    "src/graphics/OperatorLutWorker.hpp",
    "src/graphics/OperatorLutWorker.cpp",
    "src/graphics/OperatorConstants.hpp",
    "src/graphics/d3d11/ScopedD3D11PixelBindings.hpp",
    "src/graphics/d3d11/ScopedD3D11PixelBindings.cpp",
    "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.cpp",
    "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.hpp",
    "shaders/tonemap/OperatorLut.hlsli",
    "tests/shader/OperatorLutTests.cpp",
    "tests/support/reference/OperatorLutTestSupport.hpp",
    "tests/support/reference/OperatorLutTestSupport.cpp",
    "tests/support/reference/Aces2LmtSourceOracle.hpp",
    "tests/support/reference/Aces2LmtSourceOracle.cpp",
    "tests/unit/tonemap/OperatorLutTests.cpp",
    "tests/unit/graphics/HdrOperatorWorkerTests.cpp",
    "tests/shader/shaders/OperatorLut.cs.hlsl",
    "include/Fallout4HDR/config/PluginConfiguration.hpp",
    "include/Fallout4HDR/config/SettingsPersistence.hpp",
    "src/config/PluginConfiguration.cpp",
    "src/config/SettingsPersistence.cpp",
    "config/Fallout4HDR.ini",
    "tests/unit/config/HdrPersistenceTests.cpp",
    "tests/unit/config/HdrSettingsTestSupport.hpp",
    "tests/unit/config/HdrConfigurationTests.cpp",
    "src/graphics/LiveSettings.hpp",
    "include/Fallout4HDR/ui/HdrSettingsInput.hpp",
    "include/Fallout4HDR/ui/SettingsWindow.hpp",
    "src/ui/HdrSettingsInput.cpp",
    "src/ui/SettingsWindow.cpp",
    "src/ui/SettingsPanel.hpp",
    "src/ui/SettingsPanel.cpp",
    "tests/shader/SettingsPanelTests.cpp",
    "src/graphics/DeveloperExposureLock.hpp",
    "src/graphics/PipboyScreenObservation.hpp",
    "tests/unit/graphics/PipboyScreenObservationTests.cpp",
    "src/graphics/resources/DeveloperExposureResources.hpp",
    "src/graphics/resources/DeveloperExposureResources.cpp",
    "shaders/diagnostics/DeveloperExposureCapture.cs.hlsl",
    "tests/shader/DeveloperExposureTests.cpp",
    "scripts/collect-diagnostics.ps1",
    "scripts/monitor-native-session.ps1",
    "scripts/internal/NativeSession.Common.ps1",
    "tests/runtime/MonitorNativeSession.Tests.ps1",
    "scripts/internal/Project.Process.ps1",
    "scripts/internal/Project.Source.ps1",
    "tests/source/SourceArchive.Tests.ps1",
    "tests/source/StagePolicy.Tests.ps1",
    "scripts/test.ps1",
    "tests/source/TestProcess.Tests.ps1",
    "tests/shader/ShaderTestOptions.hpp",
    "tests/shader/ShaderTestOptions.cpp",
    "tests/unit/ShaderTestOptionsTests.cpp",
    "scripts/scan-runtime.ps1",
    "scripts/pin-runtime.ps1",
    "schemas/observed-runtime.schema.json",
    "tests/source/RuntimePinning.Tests.ps1",
    "include/Fallout4HDR/output/PerformanceStatistics.hpp",
    "src/output/PerformanceStatistics.cpp",
    "tests/unit/output/PerformanceTests.cpp",
    "tests/shader/PresentationTiming.hpp",
    "tests/shader/CreativeGradeLutTests.cpp", "tests/shader/HdrGradeTransferTests.cpp",
    "tests/shader/shaders/HdrGradeTransfer.cs.hlsl",
    "shaders/fallout4/ImageControls.hlsli", "tests/shader/ImageControlsTests.cpp",
    "src/graphics/resources/NativeReferenceResources.hpp",
    "src/graphics/resources/NativeReferenceResources.cpp",
    "src/graphics/passes/NativeReferenceGradePass.hpp",
    "src/graphics/passes/NativeReferenceGradePass.cpp",
    "shaders/fallout4/NativeReferencePrepare.cs.hlsl",
    "shaders/fallout4/NativeReferenceGrade.ps.hlsl",
    "shaders/fallout4/NativeReferenceGrade.hlsli",
    "tests/shader/NativeReferencePrepareTests.cpp"
)
$imguiFiles = @("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
    "imgui.h", "imgui_internal.h", "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h",
    "imstb_truetype.h", "LICENSE.txt", "backends/imgui_impl_dx11.cpp",
    "backends/imgui_impl_dx11.h", "backends/imgui_impl_win32.cpp", "backends/imgui_impl_win32.h")
$temp = [IO.Path]::Combine([IO.Path]::GetTempPath(), "Fallout4HDR-source-" + [Guid]::NewGuid())
[IO.Directory]::CreateDirectory($temp) | Out-Null
$untracked = [IO.Path]::Combine($root, "lib/commonlibf4/src/PackageSourceUntracked-" + [Guid]::NewGuid() + ".cpp")
try {
    $first = [IO.Path]::Combine($temp, "first.zip")
    $second = [IO.Path]::Combine($temp, "second.zip")
    & $script -Destination $first
    & $script -Destination $second
    $original = [IO.File]::ReadAllBytes($first)
    if ([Convert]::ToBase64String($original) -cne
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($second))) {
        throw "Repeated archives differ."
    }
    $refused = $false
    try { & $script -Destination $first }
    catch { $refused = $true }
    if (-not $refused -or [Convert]::ToBase64String($original) -cne
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($first))) {
        throw "Existing destination was not preserved."
    }

    $stream = [IO.File]::OpenRead($first)
    try {
        $zip = Open-Fo4HdrValidatedZipArchive $first
        try {
            $counts = @{ Root = 0; Shared = 0; Spdlog = 0; ImGui = 0; Project = 0 }
            $previous = ""
            foreach ($entry in $zip.Entries) {
                $name = $entry.FullName
                if ([StringComparer]::Ordinal.Compare($previous, $name) -ge 0 -or
                    $name -match '(^|/)(\.git|\.\.)($|/)' -or $name.Contains("\")) {
                    throw "Unexpected entry or order: $name"
                }
                $previous = $name
                if ($entry.LastWriteTime.DateTime -ne [DateTime]::new(1980, 1, 1)) {
                    throw "Unexpected timestamp: $name"
                }
                $sourcePath = [IO.Path]::Combine($root, $name)
                $canonical = $false
                if ($expected -ccontains $name) { ++$counts.Project }
                elseif ($name -cmatch '^lib/commonlibf4/lib/commonlib-shared/(include/.+\.h|src/.+\.cpp|src/REX/PCH\.h|LICENSE|EXCEPTIONS)$') {
                    ++$counts.Shared
                    $canonical = $true
                }
                elseif ($name -cmatch '^lib/commonlibf4/(include/.+\.h|src/.+\.cpp|LICENSE)$') {
                    ++$counts.Root
                    $canonical = $true
                }
                elseif ($name -cmatch '^lib/spdlog/(include/spdlog/.+\.h|src/(spdlog|stdout_sinks|color_sinks|file_sinks|async|cfg)\.cpp|LICENSE)$' -and
                    -not $name.StartsWith("lib/spdlog/include/spdlog/fmt/bundled/")) {
                    ++$counts.Spdlog
                    $spdlogSource = if ($env:FO4HDR_SPDLOG_SOURCE) { $env:FO4HDR_SPDLOG_SOURCE }
                        else { [IO.Path]::Combine($root, "lib/spdlog") }
                    $sourcePath = [IO.Path]::Combine($spdlogSource, $name.Substring("lib/spdlog/".Length))
                }
                elseif ($name.StartsWith("lib/imgui/", [StringComparison]::Ordinal) -and
                    $imguiFiles -ccontains $name.Substring("lib/imgui/".Length)) {
                    ++$counts.ImGui
                    $imguiSource = if ($env:FO4HDR_IMGUI_SOURCE) { $env:FO4HDR_IMGUI_SOURCE }
                        else { [IO.Path]::Combine($root, "lib/imgui") }
                    $sourcePath = [IO.Path]::Combine($imguiSource, $name.Substring("lib/imgui/".Length))
                }
                else { throw "Unselected source entry: $name" }

                $entryStream = $entry.Open()
                $memory = [IO.MemoryStream]::new()
                try {
                    $entryStream.CopyTo($memory)
                    $actual = $memory.ToArray()
                    $source = [IO.File]::ReadAllBytes($sourcePath)
                    if ($canonical) {
                        $actualText = [Text.Encoding]::UTF8.GetString($actual).Replace("`r`n", "`n")
                        $sourceText = [Text.Encoding]::UTF8.GetString($source).Replace("`r`n", "`n")
                        if ($actualText -cne $sourceText) { throw "Canonical source differs: $name" }
                    }
                    elseif ([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($source)) {
                        throw "Entry bytes differ: $name"
                    }
                }
                finally { $memory.Dispose(); $entryStream.Dispose() }
            }
            if ($counts.Project -ne $expected.Length -or $counts.Root -ne 1584 -or
                $counts.Shared -ne 110 -or $counts.Spdlog -ne 97 -or $counts.ImGui -ne 15) {
                throw "Incomplete exact source closure."
            }
            foreach ($required in @("lib/commonlibf4/include/F4SE/Impl/PCH.h",
                "lib/commonlibf4/lib/commonlib-shared/src/REX/PCH.h",
                "lib/commonlibf4/LICENSE", "lib/commonlibf4/lib/commonlib-shared/LICENSE",
                "lib/commonlibf4/lib/commonlib-shared/EXCEPTIONS", "lib/spdlog/LICENSE",
                "lib/imgui/LICENSE.txt")) {
                if (-not $zip.GetEntry($required)) { throw "Missing build or license input: $required" }
            }
        }
        finally { $zip.Dispose() }
    }
    finally { $stream.Dispose() }

    [IO.File]::WriteAllText($untracked, "Untracked source must not enter a corresponding source archive.")
    $refused = $false
    $rejected = [IO.Path]::Combine($temp, "rejected.zip")
    try { & $script -Destination $rejected }
    catch { $refused = $true }
    if (-not $refused -or [IO.File]::Exists($rejected)) {
        throw "Untracked dependency source was not rejected before archive creation."
    }
    Write-Host "PASS exact source closure, bytes, timestamps, repeatability, overwrite refusal, and untracked-source rejection"
}
finally {
    [IO.File]::Delete($untracked)
    foreach ($name in @("first.zip", "second.zip", "rejected.zip")) {
        [IO.File]::Delete([IO.Path]::Combine($temp, $name))
    }
    [IO.Directory]::Delete($temp)
}
