# SPDX-License-Identifier: GPL-3.0-or-later

param([string]$Destination, [string]$SpdlogArchive = $env:FO4HDR_SPDLOG_ARCHIVE)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression

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
    "include/Fallout4HDR/game/GameHdrSnapshot.hpp",
    "include/Fallout4HDR/output/Hdr10Color.hpp",
    "include/Fallout4HDR/runtime/RuntimeValidation.hpp",
    "scripts/package-source.ps1",
    "src/game/GameSnapshotStore.hpp",
    "src/game/GameStateObserver.cpp",
    "src/game/GameStateObserver.hpp",
    "src/game/MenuEventQueue.hpp",
    "src/output/Hdr10Color.cpp",
    "src/plugin/PluginMain.cpp",
    "src/runtime/RuntimeIdentityFiles.cpp",
    "src/runtime/RuntimeIdentityFiles.hpp",
    "src/runtime/RuntimeValidation.cpp",
    "tests/source/PackageSource.Tests.ps1",
    "tests/support/TestHarness.hpp",
    "tests/unit/main.cpp",
    "tests/unit/game/GameSnapshotStoreTests.cpp",
    "tests/unit/game/MenuEventQueueTests.cpp",
    "tests/unit/output/HdrCoreTests.cpp",
    "tests/unit/plugin/PluginBootstrapTests.cpp",
    "tests/unit/runtime/RuntimeIdentityFilesTests.cpp",
    "tests/unit/runtime/RuntimeValidationTests.cpp",
    "tests/unit/runtime/RuntimeVersionFixture.rc",
    "tests/unit/UnitTestSuite.hpp",
    "xmake.lua"
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
$spdlogSource = if ($env:FO4HDR_SPDLOG_SOURCE) { $env:FO4HDR_SPDLOG_SOURCE }
    else { [IO.Path]::Combine($root, "lib/spdlog") }
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
