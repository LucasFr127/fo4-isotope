# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

param([string]$Destination, [string]$SpdlogArchive = $env:FO4HDR_SPDLOG_ARCHIVE,
    [string]$ImguiArchive = $env:FO4HDR_IMGUI_ARCHIVE)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.Formats.Tar

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, ".."))
$files = [string[]]@(
    ".clang-format",
    ".editorconfig",
    ".gitattributes",
    ".gitignore",
    ".gitmodules",
    "COPYING.CommonLib-Exceptions",
    "COPYING.GPL-3.0-or-later",
    "THIRD_PARTY_NOTICES.md",
    "include/Fallout4HDR/config/HdrSettings.hpp",
    "include/Fallout4HDR/game/GameHdrSnapshot.hpp",
    "include/Fallout4HDR/graphics/DeviceCreationObservation.hpp",
    "include/Fallout4HDR/hooks/PeIat.hpp",
    "include/Fallout4HDR/hooks/HookOwnership.hpp",
    "include/Fallout4HDR/output/Hdr10Color.hpp",
    "include/Fallout4HDR/output/RenderPipelineContract.hpp",
    "include/Fallout4HDR/runtime/RuntimeValidation.hpp",
    "scripts/package-source.ps1",
    "src/game/GameSnapshotStore.hpp",
    "src/game/GameStateObserver.cpp",
    "src/game/GameStateObserver.hpp",
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
    "src/output/Hdr10Color.cpp",
    "src/plugin/PluginMain.cpp",
    "src/runtime/RuntimeIdentityFiles.cpp",
    "src/runtime/RuntimeIdentityFiles.hpp",
    "src/runtime/RuntimeValidation.cpp",
    "tests/source/PackageSource.Tests.ps1", "tests/source/SourceTree.Tests.ps1",
    "tests/support/TestHarness.hpp",
    "tests/unit/main.cpp",
    "tests/unit/config/HdrSettingsMathTests.cpp",
    "tests/unit/game/GameSnapshotStoreTests.cpp",
    "tests/unit/game/MenuEventQueueTests.cpp",
    "tests/unit/hooks/HookTests.cpp",
    "tests/unit/graphics/DeviceCreationHookTests.cpp",
    "tests/unit/graphics/SwapChainObservationTests.cpp",
    "tests/unit/output/HdrCoreTests.cpp",
    "tests/unit/output/PipelineContractTests.cpp",
    "tests/unit/plugin/PluginBootstrapTests.cpp",
    "tests/unit/runtime/RuntimeIdentityFilesTests.cpp",
    "tests/unit/runtime/RuntimeValidationTests.cpp",
    "tests/unit/runtime/RuntimeVersionFixture.rc",
    "tests/unit/UnitTestSuite.hpp",
    "xmake.lua",
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
    "src/graphics/DisplayDiscovery.hpp",
    "src/graphics/DisplayDiscovery.cpp",
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
    "src/ui/DisplayCalibrationPanel.hpp",
    "src/ui/DisplayCalibrationPanel.cpp",
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
    "src/graphics/resources/GpuPerformanceQueries.hpp",
    "src/graphics/resources/GpuPerformanceQueries.cpp",
    "src/graphics/passes/NativeReferenceGradePass.hpp",
    "src/graphics/passes/NativeReferenceGradePass.cpp",
    "shaders/fallout4/NativeReferencePrepare.cs.hlsl",
    "shaders/fallout4/NativeReferenceGrade.ps.hlsl",
    "shaders/fallout4/NativeReferenceGrade.hlsli",
    "tests/shader/NativeReferencePrepareTests.cpp"
)
$sources = [Collections.Generic.SortedDictionary[string, byte[]]]::new([StringComparer]::Ordinal)
foreach ($relative in $files) {
    $path = [IO.Path]::Combine($root, $relative)
    if (-not [IO.File]::Exists($path)) {
        throw "Missing source file: $relative"
    }
    if (([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Source file is a reparse point: $relative"
    }
    $sources.Add($relative, [IO.File]::ReadAllBytes($path))
}

if (-not $Destination) {
    $Destination = [IO.Path]::Combine($root, "dist/Fallout4HDR-source.zip")
}
$Destination = [IO.Path]::GetFullPath($Destination)
if ([IO.File]::Exists($Destination)) {
    throw "Destination already exists: $Destination"
}
if (-not $SpdlogArchive -or
    (Get-FileHash -LiteralPath $SpdlogArchive -Algorithm SHA256).Hash -ine
        "3d25808d2fc4db86621a46855800c99ab5734999b61c4cbf9470edf631555397") {
    throw "Supply the exact spdlog v1.16.0 ZIP through -SpdlogArchive or FO4HDR_SPDLOG_ARCHIVE."
}
if (-not $ImguiArchive -or
    (Get-FileHash -LiteralPath $ImguiArchive -Algorithm SHA256).Hash -ine
        "5e84cdaa6a6041586a0d11a3071b749734a0439d66fdbdad37ae5b27e37d396c") {
    throw "Supply the exact Dear ImGui v1.92.6-docking TAR.GZ through -ImguiArchive or FO4HDR_IMGUI_ARCHIVE."
}
$spdlogSource = if ($env:FO4HDR_SPDLOG_SOURCE) { $env:FO4HDR_SPDLOG_SOURCE }
    else { [IO.Path]::Combine($root, "lib/spdlog") }
$imguiSource = if ($env:FO4HDR_IMGUI_SOURCE) { $env:FO4HDR_IMGUI_SOURCE }
    else { [IO.Path]::Combine($root, "lib/imgui") }
$imguiFiles = @("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
    "imgui.h", "imgui_internal.h", "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h",
    "imstb_truetype.h", "LICENSE.txt", "backends/imgui_impl_dx11.cpp",
    "backends/imgui_impl_dx11.h", "backends/imgui_impl_win32.cpp", "backends/imgui_impl_win32.h")
$temporary = [IO.Path]::Combine([IO.Path]::GetTempPath(), "Fallout4HDR-dependencies-" + [Guid]::NewGuid())
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $dependencies = @(
        @{ Path = "lib/commonlibf4"; Commit = "8c734d1ec8fc16977004cd42a0242d4805efd633" },
        @{ Path = "lib/commonlibf4/lib/commonlib-shared"; Commit = "ed737c04811bf7c998f5a80340fd4ba5f9d3546b" }
    )
    foreach ($dependency in $dependencies) {
        $directory = [IO.Path]::Combine($root, $dependency.Path)
        $head = & git -C $directory rev-parse HEAD
        if ($LASTEXITCODE -ne 0 -or $head -cne $dependency.Commit) {
            throw "Unexpected dependency commit: $($dependency.Path)"
        }
        $selection = @("include", "src", "LICENSE")
        if ($dependency.Path.EndsWith("commonlib-shared")) { $selection += "EXCEPTIONS" }
        $changes = & git -C $directory status --porcelain --untracked-files=all -- $selection
        if ($LASTEXITCODE -ne 0 -or $changes) {
            throw "Dependency source is changed: $($dependency.Path)"
        }
        $ignored = & git -C $directory ls-files --others --ignored --exclude-standard -- $selection
        if ($LASTEXITCODE -ne 0 -or $ignored) {
            throw "Dependency source contains ignored additions: $($dependency.Path)"
        }
        $tree = & git -C $directory ls-tree -r $dependency.Commit -- $selection
        if ($LASTEXITCODE -ne 0 -or ($tree -cnotmatch '^100(644|755) blob [0-9a-f]{40}\t')) {
            throw "Dependency source contains non-regular Git entries: $($dependency.Path)"
        }
        $archivePath = [IO.Path]::Combine($temporary, "dependency.zip")
        & git -C $directory archive --format=zip "--output=$archivePath" $dependency.Commit -- $selection
        if ($LASTEXITCODE -ne 0) { throw "Cannot archive dependency: $($dependency.Path)" }
        $stream = [IO.File]::OpenRead($archivePath)
        try {
            $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
            try {
                foreach ($entry in $archive.Entries) {
                    $name = $entry.FullName
                    if ($name -cne "LICENSE" -and $name -cne "EXCEPTIONS" -and
                        $name -cnotmatch '^include/.+\.h$' -and $name -cnotmatch '^src/.+\.cpp$' -and
                        $name -cne "src/REX/PCH.h") { continue }
                    if ($name -match '(^|/)(\.git|\.\.)($|/)' -or $name.Contains("\")) {
                        throw "Unsafe dependency entry: $name"
                    }
                    $entryStream = $entry.Open()
                    $memory = [IO.MemoryStream]::new()
                    try {
                        $entryStream.CopyTo($memory)
                        $sources.Add($dependency.Path + "/" + $name, $memory.ToArray())
                    }
                    finally { $memory.Dispose(); $entryStream.Dispose() }
                }
            }
            finally { $archive.Dispose() }
        }
        finally { $stream.Dispose() }
        [IO.File]::Delete($archivePath)
    }

    $stream = [IO.File]::OpenRead($SpdlogArchive)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            foreach ($entry in $archive.Entries) {
                if (-not $entry.FullName.StartsWith("spdlog-1.16.0/", [StringComparison]::Ordinal)) { continue }
                $name = $entry.FullName.Substring("spdlog-1.16.0/".Length)
                if ($name -cne "LICENSE" -and
                    ($name -cnotmatch '^include/spdlog/.+\.h$' -or $name.StartsWith("include/spdlog/fmt/bundled/")) -and
                    $name -cnotmatch '^src/(spdlog|stdout_sinks|color_sinks|file_sinks|async|cfg)\.cpp$') { continue }
                $entryStream = $entry.Open()
                $memory = [IO.MemoryStream]::new()
                try {
                    $entryStream.CopyTo($memory)
                    $bytes = $memory.ToArray()
                    $local = [IO.Path]::Combine($spdlogSource, $name)
                    if (-not [IO.File]::Exists($local) -or
                        [Convert]::ToBase64String($bytes) -cne
                        [Convert]::ToBase64String([IO.File]::ReadAllBytes($local))) {
                        throw "spdlog source differs from exact ZIP: $name"
                    }
                    $sources.Add("lib/spdlog/" + $name, $bytes)
                }
                finally { $memory.Dispose(); $entryStream.Dispose() }
            }
        }
        finally { $archive.Dispose() }
    }
    finally { $stream.Dispose() }
    $stream = [IO.File]::OpenRead($ImguiArchive)
    try {
        $gzip = [IO.Compression.GZipStream]::new($stream, [IO.Compression.CompressionMode]::Decompress, $true)
        try {
            $tar = [Formats.Tar.TarReader]::new($gzip, $true)
            try {
                $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
                while ($null -ne ($entry = $tar.GetNextEntry())) {
                    $prefix = "imgui-1.92.6-docking/"
                    if (-not $entry.Name.StartsWith($prefix, [StringComparison]::Ordinal)) { continue }
                    $name = $entry.Name.Substring($prefix.Length)
                    if ($imguiFiles -cnotcontains $name) { continue }
                    if ($entry.EntryType -ne [Formats.Tar.TarEntryType]::RegularFile -or
                        -not $seen.Add($name)) {
                        throw "Non-regular or duplicate Dear ImGui member: $name"
                    }
                    $memory = [IO.MemoryStream]::new()
                    try {
                        $entry.DataStream.CopyTo($memory)
                        $bytes = $memory.ToArray()
                        $local = [IO.Path]::Combine($imguiSource, $name)
                        if (-not [IO.File]::Exists($local) -or
                            ([IO.File]::GetAttributes($local) -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                            [Convert]::ToBase64String($bytes) -cne
                            [Convert]::ToBase64String([IO.File]::ReadAllBytes($local))) {
                            throw "Dear ImGui source differs from exact TAR.GZ: $name"
                        }
                        $sources.Add("lib/imgui/" + $name, $bytes)
                    }
                    finally { $memory.Dispose() }
                }
                if ($seen.Count -ne $imguiFiles.Length) { throw "Incomplete Dear ImGui core/backend source." }
            }
            finally { $tar.Dispose() }
        }
        finally { $gzip.Dispose() }
    }
    finally { $stream.Dispose() }
}
finally {
    [IO.File]::Delete([IO.Path]::Combine($temporary, "dependency.zip"))
    [IO.Directory]::Delete($temporary)
}

[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Destination)) | Out-Null
$timestamp = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$stream = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$complete = $false
try {
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($source in $sources.GetEnumerator()) {
            $entry = $zip.CreateEntry($source.Key, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $timestamp
            $entryStream = $entry.Open()
            try { $entryStream.Write($source.Value, 0, $source.Value.Length) }
            finally { $entryStream.Dispose() }
        }
    }
    finally { $zip.Dispose() }
    $complete = $true
}
finally {
    $stream.Dispose()
    if (-not $complete) { [IO.File]::Delete($Destination) }
}
Write-Host "Source package: $Destination"
