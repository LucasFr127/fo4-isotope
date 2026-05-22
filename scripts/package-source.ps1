# SPDX-License-Identifier: GPL-3.0-or-later

param([string]$Destination)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, ".."))
$files = [string[]]@(
    ".gitattributes",
    ".gitignore",
    "COPYING.GPL-3.0-or-later",
    "include/Fallout4HDR/output/Hdr10Color.hpp",
    "scripts/package-source.ps1",
    "src/output/Hdr10Color.cpp",
    "tests/source/PackageSource.Tests.ps1",
    "tests/support/TestHarness.hpp",
    "tests/unit/output/HdrCoreTests.cpp",
    "xmake.lua"
)
[Array]::Sort($files, [StringComparer]::Ordinal)
foreach ($relative in $files) {
    if (-not [IO.File]::Exists([IO.Path]::Combine($root, $relative))) {
        throw "Missing source file: $relative"
    }
}

if (-not $Destination) {
    $Destination = [IO.Path]::Combine($root, "dist/Fallout4HDR-source.zip")
}
$Destination = [IO.Path]::GetFullPath($Destination)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Destination)) | Out-Null
$timestamp = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$stream = [IO.File]::Open(
    $Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$complete = $false
try {
    $zip = [IO.Compression.ZipArchive]::new(
        $stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($relative in $files) {
            $bytes = [IO.File]::ReadAllBytes([IO.Path]::Combine($root, $relative))
            $entry = $zip.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $timestamp
            $entryStream = $entry.Open()
            try {
                $entryStream.Write($bytes, 0, $bytes.Length)
            }
            finally {
                $entryStream.Dispose()
            }
        }
    }
    finally {
        $zip.Dispose()
    }
    $complete = $true
}
finally {
    $stream.Dispose()
    if (-not $complete) {
        [IO.File]::Delete($Destination)
    }
}
Write-Host "Source package: $Destination"
