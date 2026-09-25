# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#requires -Version 7.4.6

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
. ([IO.Path]::Combine($PSScriptRoot, "../../scripts/internal/Project.Source.ps1"))

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, "../.."))
$script = [IO.Path]::Combine($root, "scripts/package-source.ps1")
$expected = [string[]]@(
    ".clang-format",
    ".editorconfig",
    ".gitattributes",
    ".gitignore",
    ".gitmodules",
    "config/Fallout4HDR.ini",
    "COPYING.CommonLib-Exceptions",
    "COPYING.GPL-3.0-or-later",
    "include/Fallout4HDR/config/HdrSettings.hpp",
    "include/Fallout4HDR/config/PluginConfiguration.hpp",
    "include/Fallout4HDR/config/SettingsPersistence.hpp",
    "include/Fallout4HDR/game/GameHdrSnapshot.hpp",
    "include/Fallout4HDR/graphics/DeviceCreationObservation.hpp",
    "include/Fallout4HDR/hooks/HookOwnership.hpp",
    "include/Fallout4HDR/hooks/PeIat.hpp",
    "include/Fallout4HDR/output/ArmorUiComposition.hpp",
    "include/Fallout4HDR/output/Hdr10Color.hpp",
    "include/Fallout4HDR/output/NativeLutObservation.hpp",
    "include/Fallout4HDR/output/PerformanceStatistics.hpp",
    "include/Fallout4HDR/output/RenderPipelineContract.hpp",
    "include/Fallout4HDR/output/SelectiveColor.hpp",
    "include/Fallout4HDR/runtime/RuntimeValidation.hpp",
    "include/Fallout4HDR/tonemap/Aces2.hpp",
    "include/Fallout4HDR/tonemap/Aces2Lmt.hpp",
    "include/Fallout4HDR/tonemap/Agx.hpp",
    "include/Fallout4HDR/tonemap/Gt7.hpp",
    "include/Fallout4HDR/tonemap/OperatorLut.hpp",
    "include/Fallout4HDR/ui/HdrSettingsInput.hpp",
    "include/Fallout4HDR/ui/SettingsWindow.hpp",
    "LICENSES/Apache-2.0.txt",
    "LICENSES/BSD-3-Clause.txt",
    "runtime/1.10.163/manifest.json",
    "runtime/1.10.163/PluginVersion.cpp",
    "runtime/1.10.163/RuntimeIdentity.generated.hpp",
    "runtime/1.10.984/manifest.json",
    "runtime/1.10.984/PluginVersion.cpp",
    "runtime/1.11.221/manifest.json",
    "runtime/1.11.221/PluginVersion.cpp",
    "schemas/observed-runtime.schema.json",
    "schemas/runtime-manifest.schema.json",
    "schemas/versions-lock.schema.json",
    "schemas/xmake-source-manifest.schema.json",
    "scripts/add-commonlib.ps1",
    "scripts/audit-f4se-project.ps1",
    "scripts/bootstrap.ps1",
    "scripts/build.ps1",
    "scripts/collect-diagnostics.ps1",
    "scripts/compile-shaders.ps1",
    "scripts/configure.ps1",
    "scripts/deploy.ps1",
    "scripts/fetch-gt7-reference.ps1",
    "scripts/generate-runtime-identity.ps1",
    "scripts/internal/CommonLibIntegrity.ps1",
    "scripts/internal/NativeSession.Common.ps1",
    "scripts/internal/Project.Common.ps1",
    "scripts/internal/Project.Files.ps1",
    "scripts/internal/Project.Process.ps1",
    "scripts/internal/Project.Runtime.Binary.ps1",
    "scripts/internal/Project.Runtime.Profiles.ps1",
    "scripts/internal/Project.Runtime.ps1",
    "scripts/internal/Project.Source.ps1",
    "scripts/internal/Project.Toolchain.ps1",
    "scripts/monitor-native-session.ps1",
    "scripts/package-source.ps1",
    "scripts/pin-runtime.ps1",
    "scripts/pin-toolchain.ps1",
    "scripts/query-commonlib-head.ps1",
    "scripts/scan-runtime.ps1",
    "scripts/test.ps1",
    "scripts/validate-manifests.ps1",
    "scripts/validate-runtime.ps1",
    "scripts/verify-plugin-metadata.ps1",
    "shaders/armor/EncodedComposition.cs.hlsl",
    "shaders/diagnostics/DeveloperExposureCapture.cs.hlsl",
    "shaders/fallout4/CreativeGradeLut.ps.hlsl",
    "shaders/fallout4/Fxaa.ps.hlsl",
    "shaders/fallout4/GradeTransfer.hlsli",
    "shaders/fallout4/ImageControls.hlsli",
    "shaders/fallout4/NativeReferenceGrade.hlsli",
    "shaders/fallout4/NativeReferenceGrade.ps.hlsl",
    "shaders/fallout4/NativeReferencePrepare.cs.hlsl",
    "shaders/fallout4/SelectiveColor.hlsli",
    "shaders/fallout4/TemporalAa.ps.hlsl",
    "shaders/fallout4/TemporalAa2.ps.hlsl",
    "shaders/fallout4/TemporalIntermediate.hlsli",
    "shaders/fallout4/ToneMapA.ps.hlsl",
    "shaders/fallout4/ToneMapB.ps.hlsl",
    "shaders/fallout4/ToneMapBypassCommon.hlsli",
    "shaders/fallout4/ToneMapNewA.ps.hlsl",
    "shaders/fallout4/ToneMapNewB.ps.hlsl",
    "shaders/fallout4/Video.ps.hlsl",
    "shaders/output/DisplayMath.hlsli",
    "shaders/output/PresentPixels.cs.hlsl",
    "shaders/output/SceneLayers.hlsli",
    "shaders/tonemap/Gt7.hlsli",
    "shaders/tonemap/OperatorLut.hlsli",
    "src/config/HdrSettings.cpp",
    "src/config/PluginConfiguration.cpp",
    "src/config/SettingsPersistence.cpp",
    "src/game/ArmorUiObserver.cpp",
    "src/game/ArmorUiObserver.hpp",
    "src/game/DirectCallPatch.hpp",
    "src/game/GameObservationMemory.hpp",
    "src/game/GameSnapshotStore.hpp",
    "src/game/GameStateObserver.cpp",
    "src/game/GameStateObserver.hpp",
    "src/game/ImageSpaceObserver.cpp",
    "src/game/ImageSpaceObserver.hpp",
    "src/game/ImageSpaceState.cpp",
    "src/game/MenuEventQueue.hpp",
    "src/game/NativeLutObserver.cpp",
    "src/game/NativeLutObserver.hpp",
    "src/game/RendererSnapshot.cpp",
    "src/graphics/d3d11/ContextQuerySafety.cpp",
    "src/graphics/d3d11/ContextQuerySafety.hpp",
    "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.cpp",
    "src/graphics/d3d11/ScopedD3D11ComputeConstantBuffer.hpp",
    "src/graphics/d3d11/ScopedD3D11PixelBindings.cpp",
    "src/graphics/d3d11/ScopedD3D11PixelBindings.hpp",
    "src/graphics/DeveloperExposureLock.hpp",
    "src/graphics/DisplayDiscovery.cpp",
    "src/graphics/DisplayDiscovery.hpp",
    "src/graphics/HdrRenderer.cpp",
    "src/graphics/hooks/ContextHooks.cpp",
    "src/graphics/hooks/ContextHooks.hpp",
    "src/graphics/hooks/DeviceHooks.cpp",
    "src/graphics/hooks/DeviceHooks.hpp",
    "src/graphics/hooks/HookRecords.hpp",
    "src/graphics/hooks/SwapChainHooks.cpp",
    "src/graphics/hooks/SwapChainHooks.hpp",
    "src/graphics/LiveSettings.hpp",
    "src/graphics/OperatorConstants.hpp",
    "src/graphics/OperatorLutWorker.cpp",
    "src/graphics/OperatorLutWorker.hpp",
    "src/graphics/passes/ArmorCompositionPass.cpp",
    "src/graphics/passes/ArmorCompositionPass.hpp",
    "src/graphics/passes/ArmorEncodedComposition.cpp",
    "src/graphics/passes/ArmorEncodedComposition.hpp",
    "src/graphics/passes/NativeReferenceGradePass.cpp",
    "src/graphics/passes/NativeReferenceGradePass.hpp",
    "src/graphics/passes/ObservedDraw.cpp",
    "src/graphics/passes/ObservedDraw.hpp",
    "src/graphics/passes/PresentationPass.cpp",
    "src/graphics/passes/PresentationPass.hpp",
    "src/graphics/PipboyScreenObservation.hpp",
    "src/graphics/RendererLifetime.hpp",
    "src/graphics/RenoDxShaderIdentity.hpp",
    "src/graphics/resources/DeveloperExposureResources.cpp",
    "src/graphics/resources/DeveloperExposureResources.hpp",
    "src/graphics/resources/HdrResourcePolicy.hpp",
    "src/graphics/resources/NativeReferenceResources.cpp",
    "src/graphics/resources/NativeReferenceResources.hpp",
    "src/hooks/HookOwnership.cpp",
    "src/hooks/PeIat.cpp",
    "src/output/ArmorUiComposition.cpp",
    "src/output/Hdr10Color.cpp",
    "src/output/NativeLutObservation.cpp",
    "src/output/PerformanceStatistics.cpp",
    "src/output/SelectiveColor.cpp",
    "src/plugin/PluginMain.cpp",
    "src/plugin/PluginPch.hpp",
    "src/runtime/RuntimeIdentityFiles.cpp",
    "src/runtime/RuntimeIdentityFiles.hpp",
    "src/runtime/RuntimeValidation.cpp",
    "src/tonemap/Aces2.cpp",
    "src/tonemap/aces2/Aces2ColorAppearance.cpp",
    "src/tonemap/aces2/Aces2Model.hpp",
    "src/tonemap/aces2/Aces2Tables.cpp",
    "src/tonemap/Aces2Lmt.cpp",
    "src/tonemap/Agx.cpp",
    "src/tonemap/Gt7.cpp",
    "src/tonemap/OperatorLut.cpp",
    "src/ui/DisplayCalibrationPanel.cpp",
    "src/ui/DisplayCalibrationPanel.hpp",
    "src/ui/HdrSettingsInput.cpp",
    "src/ui/LutProfilesPanel.cpp",
    "src/ui/LutProfilesPanel.hpp",
    "src/ui/SettingsPanel.cpp",
    "src/ui/SettingsPanel.hpp",
    "src/ui/SettingsWindow.cpp",
    "tests/runtime/AddressLibraryV0.Tests.ps1",
    "tests/runtime/CommonLibIntegrity.Tests.ps1",
    "tests/runtime/CommonLibRelocationProfile.Tests.ps1",
    "tests/runtime/MonitorNativeSession.Tests.ps1",
    "tests/runtime/RuntimeStatusPolicy.Tests.ps1",
    "tests/shader/Gt7ReferenceTests.cpp",
    "tests/shader/Gt7ReferenceTests.hpp",
    "tests/shader/HdrGradeTransferTests.cpp",
    "tests/shader/main.cpp",
    "tests/shader/OperatorLutTests.cpp",
    "tests/shader/PresentationTiming.hpp",
    "tests/shader/SettingsPanelTests.cpp",
    "tests/shader/shaders/ArmorEncodedSource.ps.hlsl",
    "tests/shader/shaders/Fullscreen.vs.hlsl",
    "tests/shader/shaders/HdrGradeTransfer.cs.hlsl",
    "tests/shader/shaders/OperatorLut.cs.hlsl",
    "tests/shader/shaders/ScenePassSentinel.ps.hlsl",
    "tests/shader/shaders/TransferEvaluation.cs.hlsl",
    "tests/shader/ShaderTestOptions.cpp",
    "tests/shader/ShaderTestOptions.hpp",
    "tests/shader/ShaderTestSuite.hpp",
    "tests/source/ArchitectureLayout.Tests.ps1",
    "tests/source/PackageSource.Tests.ps1",
    "tests/source/PluginBoundarySource.Tests.ps1",
    "tests/source/SourceArchive.Tests.ps1",
    "tests/source/SourceText.Tests.ps1",
    "tests/source/SourceTree.Tests.ps1",
    "tests/source/StagePolicy.Tests.ps1",
    "tests/source/TestProcess.Tests.ps1",
    "tests/source/ToolchainPolicy.Tests.ps1",
    "tests/support/d3d11/GpuHarness.cpp",
    "tests/support/d3d11/GpuHarness.hpp",
    "tests/support/reference/Aces2LmtSourceOracle.cpp",
    "tests/support/reference/Aces2LmtSourceOracle.hpp",
    "tests/support/reference/OperatorLutTestSupport.cpp",
    "tests/support/reference/OperatorLutTestSupport.hpp",
    "tests/support/reference/SelectiveColorSourceOracle.cpp",
    "tests/support/reference/SelectiveColorSourceOracle.hpp",
    "tests/support/TestHarness.hpp",
    "tests/unit/config/HdrConfigurationTests.cpp",
    "tests/unit/config/HdrGradeProfileTests.cpp",
    "tests/unit/config/HdrPersistenceTests.cpp",
    "tests/unit/config/HdrSelectiveColorTests.cpp",
    "tests/unit/config/HdrSettingsMathTests.cpp",
    "tests/unit/config/HdrSettingsTestSupport.hpp",
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
    "tests/unit/ui/LutProfilesPanelTests.cpp",
    "tests/unit/UnitTestSuite.hpp",
    "THIRD_PARTY_NOTICES.md",
    "third_party/gt7/UPSTREAM.lock.json",
    "third_party/gt7/upstream/gt7_tone_mapping.cpp",
    "third_party/selective-color/UPSTREAM.lock.json",
    "third_party/xmake-source-manifest.json",
    "versions.lock.json",
    "xmake-requires.lock",
    "xmake.lua",
    "config/Fallout4HDR/Default.ini",
    "src/hooks/GraphicsHookSupport.cpp",
    "include/Fallout4HDR/graphics/HdrRenderer.hpp",
    "src/graphics/DrawObservation.hpp",
    "src/graphics/GraphicsDiagnostics.cpp",
    "src/graphics/GraphicsDiagnostics.hpp",
    "src/graphics/PresentFrame.cpp",
    "src/graphics/PresentFrame.hpp",
    "src/graphics/ShaderRegistry.cpp",
    "src/graphics/ShaderRegistry.hpp",
    "src/graphics/d3d11/BorrowedComputeState.cpp",
    "src/graphics/d3d11/BorrowedComputeState.hpp",
    "src/graphics/resources/PresentationResources.cpp",
    "src/graphics/resources/PresentationResources.hpp",
    "src/graphics/resources/ReplacementShaders.cpp",
    "src/graphics/resources/ReplacementShaders.hpp",
    "src/graphics/resources/SceneResources.cpp",
    "src/graphics/resources/SceneResources.hpp",
    "src/graphics/resources/ArmorCompositionResources.hpp",
    "src/graphics/resources/ArmorCompositionResources.cpp",
    "src/graphics/LutProfileBindings.hpp",
    "shaders/fallout4/LiveSettingsConstants.hlsli",
    "include/Fallout4HDR/output/HdrActivationPolicy.hpp",
    "src/output/HdrActivationPolicy.cpp",
    "src/output/RenderPipelineContract.cpp",
    "include/Fallout4HDR/game/ImageSpaceSemantic.hpp",
    "src/config/SettingsPersistence.hpp",
    "tests/unit/game/GameStateObserverTests.cpp",
    "tests/unit/TestHarnessTests.cpp",
    "tests/shader/ArmorCompositionTests.cpp",
    "tests/shader/CreativeGradeLutTests.cpp",
    "tests/shader/DeveloperExposureLockTests.cpp",
    "tests/shader/Gt7OperatorUiTests.cpp",
    "tests/shader/ImageControlsTests.cpp",
    "tests/shader/NativeReferenceGradeTests.cpp",
    "tests/shader/PresentationPassTests.cpp",
    "tests/shader/SeparatedUiTests.cpp",
    "tests/shader/shaders/LayerEvaluation.cs.hlsl",
    "tests/shader/shaders/NativeGradeSampling.ps.hlsl",
    "tests/shader/ShippingPixelShaderTests.cpp",
    "tests/shader/ShippingPixelShaderTests.hpp",
    "tests/shader/StateRestorationTests.cpp",
    "tests/support/reference/ImageControlReference.cpp",
    "tests/support/reference/ImageControlReference.hpp",
    "tests/support/reference/NativeGradeReference.cpp",
    "tests/support/reference/NativeGradeReference.hpp",
    "tests/support/reference/ShaderPixels.hpp",
    "tests/support/reference/ShaderReference.cpp",
    "tests/support/reference/ShaderReference.hpp",
    "scripts/internal/SourceText.ps1",
    "tests/runtime/RuntimePinning.Tests.ps1",
    "tests/unit/output/ArmorUiCompositionTests.cpp",
    "tests/unit/output/NativeLutObservationTests.cpp",
    "tests/unit/graphics/DeveloperExposureLockTests.cpp",
    "tests/unit/ui/DisplayCalibrationTests.cpp",
    "tests/unit/config/HdrGradePackTests.cpp",
    "tests/runtime/ImageSpaceProfile.Tests.ps1",
    "tests/unit/graphics/ScopedD3D11ComputeConstantBufferTests.cpp",
    "local.env.example.ps1",
    "scripts/generate-combined-license.ps1",
    "LICENSES/MIT.txt",
    "REUSE.toml",
    "LICENSES/LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions.txt"
)
$imguiFiles = @("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
    "imgui.h", "imgui_internal.h", "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h",
    "imstb_truetype.h", "LICENSE.txt", "backends/imgui_impl_dx11.cpp",
    "backends/imgui_impl_dx11.h", "backends/imgui_impl_win32.cpp", "backends/imgui_impl_win32.h")
function Get-LockedPackageArtifactPath {
    param([string]$Name)

    $filename = $Name.Substring('third_party/xmake-package-sources/'.Length)
    $manifest = Get-Content -LiteralPath ([IO.Path]::Combine($root,
        'third_party/xmake-source-manifest.json')) -Raw | ConvertFrom-Json
    foreach ($package in $manifest.packages) {
        if ($filename -ceq $package.sourceCacheFile) {
            return $(if ($package.name -ceq 'spdlog') { $env:FO4HDR_SPDLOG_ARCHIVE } else { $env:FO4HDR_IMGUI_ARCHIVE })
        }
        if ($filename -ceq $package.recipeCacheFile) {
            return [IO.Path]::Combine($env:FO4HDR_SOURCE_CACHE, $filename)
        }
    }
    throw 'Unselected package source artifact.'
}

function Get-SourceEntryRole {
    param([string]$Name)

    $sourcePath = [IO.Path]::Combine($root, $name)
    if ($expected -ccontains $name) {
        return @{ kind = 'Project'; path = $sourcePath; canonical = $false }
    }
    foreach ($role in @(
        @{ kind = 'Shared'; pattern = '^lib/commonlibf4/lib/commonlib-shared/(include/.+\.h|src/.+\.cpp|src/REX/PCH\.h|LICENSE|EXCEPTIONS|xmake\.lua|res/[^/]+\.(rc|cpp)\.in)$' },
        @{ kind = 'UiApi'; pattern = '^lib/commonlibf4/lib/dearmoddingui-api/(include/.+\.h|LICENSE|xmake\.lua)$' },
        @{ kind = 'Root'; pattern = '^lib/commonlibf4/(include/.+\.h|src/.+\.cpp|LICENSE|EXCEPTIONS|res/license/MIT|res/commonlibf4\.natvis|xmake\.lua|res/[^/]+\.(rc|cpp)\.in)$' }
    )) {
        if ($name -cmatch $role.pattern) {
            return @{ kind = $role.kind; path = $sourcePath; canonical = $true }
        }
    }
    return Get-PackageEntryRole $name
}

function Get-PackageEntryRole {
    param([string]$Name)

    if ($name.StartsWith('third_party/xmake-package-sources/', [StringComparison]::Ordinal)) {
        return @{ kind = 'PackageArtifacts'; path = (Get-LockedPackageArtifactPath $name); canonical = $false }
    }
    if ($name -cmatch '^lib/spdlog/(include/spdlog/.+\.h|src/(spdlog|stdout_sinks|color_sinks|file_sinks|async|cfg)\.cpp|LICENSE)$' -and
        -not $name.StartsWith("lib/spdlog/include/spdlog/fmt/bundled/")) {
        $source = Get-PackageSourceRoot 'FO4HDR_SPDLOG_SOURCE' 'lib/spdlog'
        return @{ kind = 'Spdlog'; path = [IO.Path]::Combine($source, $name.Substring("lib/spdlog/".Length)); canonical = $false }
    }
    if ($name.StartsWith("lib/imgui/", [StringComparison]::Ordinal) -and
        $imguiFiles -ccontains $name.Substring("lib/imgui/".Length)) {
        $source = Get-PackageSourceRoot 'FO4HDR_IMGUI_SOURCE' 'lib/imgui'
        return @{ kind = 'ImGui'; path = [IO.Path]::Combine($source, $name.Substring("lib/imgui/".Length)); canonical = $false }
    }
    throw "Unselected source entry: $name"
}

function Get-PackageSourceRoot {
    param([string]$Variable, [string]$Fallback)

    $source = [Environment]::GetEnvironmentVariable($Variable)
    if ($source) { return $source }
    return [IO.Path]::Combine($root, $Fallback)
}

function Assert-EntryCounts {
    param([hashtable]$Counts)

    $required = @{ Project = $expected.Length; Root = 1800; Shared = 127; UiApi = 16;
        PackageArtifacts = 4; Spdlog = 97; ImGui = 15 }
    foreach ($kind in $required.Keys) {
        if ($Counts[$kind] -ne $required[$kind]) { throw "Incomplete exact source closure: $kind" }
    }
}

function Assert-SourceEntryBytes {
    param([byte[]]$Actual, [byte[]]$Source, [bool]$Canonical, [string]$Name)

    if ($canonical) {
        $actualText = [Text.Encoding]::UTF8.GetString($actual).Replace("`r`n", "`n")
        $sourceText = [Text.Encoding]::UTF8.GetString($source).Replace("`r`n", "`n")
        if ($actualText -cne $sourceText) { throw "Canonical source differs: $name" }
    }
    elseif ([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($source)) {
        throw "Entry bytes differ: $name"
    }
}

function Assert-ArchiveEntryHeader {
    param($Entry, [string]$Previous)

    $name = $Entry.FullName
    if ([StringComparer]::Ordinal.Compare($previous, $name) -ge 0) {
        throw "Unexpected entry order: $name"
    }
    if ($name -match '(^|/)(\.git|\.\.)($|/)' -or $name.Contains("\")) {
        throw "Unsafe source entry: $name"
    }
    if ($entry.LastWriteTime.DateTime -ne [DateTime]::new(1980, 1, 1)) {
        throw "Unexpected timestamp: $name"
    }
}

function Assert-RequiredArchiveInputs {
    param($Zip)

    foreach ($required in @("lib/commonlibf4/include/F4SE/Impl/PCH.h",
        "lib/commonlibf4/lib/commonlib-shared/src/REX/PCH.h",
        "lib/commonlibf4/LICENSE", "lib/commonlibf4/EXCEPTIONS", "lib/commonlibf4/res/license/MIT",
        "lib/commonlibf4/res/commonlibf4.natvis", "lib/commonlibf4/xmake.lua", "lib/commonlibf4/lib/commonlib-shared/xmake.lua",
        "lib/commonlibf4/lib/commonlib-shared/res/commonlib-plugin.rc.in",
        "lib/commonlibf4/lib/dearmoddingui-api/xmake.lua",
        "lib/commonlibf4/lib/dearmoddingui-api/LICENSE",
        "lib/commonlibf4/lib/commonlib-shared/LICENSE",
        "lib/commonlibf4/lib/commonlib-shared/EXCEPTIONS", "lib/spdlog/LICENSE",
        "lib/imgui/LICENSE.txt")) {
        if (-not $zip.GetEntry($required)) { throw "Missing build or license input: $required" }
    }
}

function Assert-SourceArchive {
    param([string]$First)

    $stream = [IO.File]::OpenRead($first)
    try {
        $zip = Open-Fo4HdrValidatedZipArchive $first
        try {
            $counts = @{ Root = 0; Shared = 0; UiApi = 0; PackageArtifacts = 0; Spdlog = 0; ImGui = 0; Project = 0 }
            $previous = ""
            foreach ($entry in $zip.Entries) {
                $name = $entry.FullName
                Assert-ArchiveEntryHeader $entry $previous
                $previous = $name
                $role = Get-SourceEntryRole $name
                ++$counts[$role.kind]
                $sourcePath = $role.path
                $canonical = $role.canonical

                $entryStream = $entry.Open()
                $memory = [IO.MemoryStream]::new()
                try {
                    $entryStream.CopyTo($memory)
                    $actual = $memory.ToArray()
                    $source = [IO.File]::ReadAllBytes($sourcePath)
                    Assert-SourceEntryBytes $actual $source $canonical $name
                }
                finally { $memory.Dispose(); $entryStream.Dispose() }
            }
            Assert-EntryCounts $counts
            Assert-RequiredArchiveInputs $zip
        }
        finally { $zip.Dispose() }
    }
    finally { $stream.Dispose() }

}

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

    Assert-SourceArchive $first

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
