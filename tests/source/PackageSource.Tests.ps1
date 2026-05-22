# SPDX-License-Identifier: GPL-3.0-or-later

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, "../.."))
$script = [IO.Path]::Combine($root, "scripts/package-source.ps1")
$expected = [string[]]@(
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
[Array]::Sort($expected, [StringComparer]::Ordinal)
$temp = [IO.Path]::Combine([IO.Path]::GetTempPath(), "Fallout4HDR-source-" + [Guid]::NewGuid())
[IO.Directory]::CreateDirectory($temp) | Out-Null
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
    try {
        & $script -Destination $first
    }
    catch {
        $refused = $true
    }
    if (-not $refused -or [Convert]::ToBase64String($original) -cne
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($first))) {
        throw "Existing destination was not preserved."
    }

    $stream = [IO.File]::OpenRead($first)
    try {
        $zip = [IO.Compression.ZipArchive]::new(
            $stream, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            if ($zip.Entries.Count -ne $expected.Length) {
                throw "Unexpected archive entry count."
            }
            for ($index = 0; $index -lt $expected.Length; ++$index) {
                $entry = $zip.Entries[$index]
                if ($entry.FullName -cne $expected[$index]) {
                    throw "Unexpected entry or order: $($entry.FullName)"
                }
                if ($entry.LastWriteTime.DateTime -ne [DateTime]::new(1980, 1, 1)) {
                    throw "Unexpected timestamp: $($entry.FullName)"
                }
                $entryStream = $entry.Open()
                $memory = [IO.MemoryStream]::new()
                try {
                    $entryStream.CopyTo($memory)
                    $source = [IO.File]::ReadAllBytes([IO.Path]::Combine($root, $expected[$index]))
                    if ([Convert]::ToBase64String($memory.ToArray()) -cne
                        [Convert]::ToBase64String($source)) {
                        throw "Entry bytes differ: $($entry.FullName)"
                    }
                }
                finally {
                    $memory.Dispose()
                    $entryStream.Dispose()
                }
            }
        }
        finally {
            $zip.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
    Write-Host "PASS source package entries, bytes, timestamps, repeatability, and overwrite refusal"
}
finally {
    [IO.File]::Delete([IO.Path]::Combine($temp, "first.zip"))
    [IO.File]::Delete([IO.Path]::Combine($temp, "second.zip"))
    [IO.Directory]::Delete($temp)
}
