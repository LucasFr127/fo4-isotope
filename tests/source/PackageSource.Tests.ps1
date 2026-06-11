# SPDX-License-Identifier: GPL-3.0-or-later

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, "../.."))
$script = [IO.Path]::Combine($root, "scripts/package-source.ps1")
$expected = [string[]]@(
    ".clang-format", ".editorconfig", ".gitattributes", ".gitignore", ".gitmodules",
    "COPYING.CommonLib-Exceptions", "COPYING.GPL-3.0-or-later",
    "THIRD_PARTY_NOTICES.md",
    "include/Fallout4HDR/config/HdrSettings.hpp",
    "include/Fallout4HDR/game/GameHdrSnapshot.hpp",
    "include/Fallout4HDR/hooks/PeIat.hpp",
    "include/Fallout4HDR/hooks/HookOwnership.hpp",
    "include/Fallout4HDR/output/Hdr10Color.hpp",
    "include/Fallout4HDR/runtime/RuntimeValidation.hpp",
    "scripts/package-source.ps1", "src/game/GameSnapshotStore.hpp",
    "src/game/GameStateObserver.cpp", "src/game/GameStateObserver.hpp",
    "src/game/MenuEventQueue.hpp",
    "src/hooks/PeIat.cpp",
    "src/hooks/HookOwnership.cpp",
    "src/config/HdrSettings.cpp",
    "src/output/Hdr10Color.cpp", "src/plugin/PluginMain.cpp",
    "src/runtime/RuntimeIdentityFiles.cpp", "src/runtime/RuntimeIdentityFiles.hpp",
    "src/runtime/RuntimeValidation.cpp", "tests/source/PackageSource.Tests.ps1",
    "tests/support/TestHarness.hpp", "tests/unit/main.cpp", "tests/unit/output/HdrCoreTests.cpp",
    "tests/unit/config/HdrSettingsMathTests.cpp",
    "tests/unit/game/GameSnapshotStoreTests.cpp",
    "tests/unit/game/MenuEventQueueTests.cpp",
    "tests/unit/hooks/HookTests.cpp",
    "tests/unit/plugin/PluginBootstrapTests.cpp", "tests/unit/runtime/RuntimeIdentityFilesTests.cpp",
    "tests/unit/runtime/RuntimeValidationTests.cpp", "tests/unit/runtime/RuntimeVersionFixture.rc",
    "tests/unit/UnitTestSuite.hpp", "xmake.lua",
    "tests/shader/main.cpp",
    "tests/shader/shaders/TransferEvaluation.cs.hlsl",
    "tests/support/d3d11/GpuHarness.hpp",
    "tests/support/d3d11/GpuHarness.cpp",
    "scripts/compile-shaders.ps1"
)
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
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            $counts = @{ Root = 0; Shared = 0; Spdlog = 0; Project = 0 }
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
                $counts.Shared -ne 110 -or $counts.Spdlog -ne 97) {
                throw "Incomplete exact source closure."
            }
            foreach ($required in @("lib/commonlibf4/include/F4SE/Impl/PCH.h",
                "lib/commonlibf4/lib/commonlib-shared/src/REX/PCH.h",
                "lib/commonlibf4/LICENSE", "lib/commonlibf4/lib/commonlib-shared/LICENSE",
                "lib/commonlibf4/lib/commonlib-shared/EXCEPTIONS", "lib/spdlog/LICENSE")) {
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
