# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot "scripts/internal/Project.Common.ps1")
$archivePath = Join-Path ([IO.Path]::GetTempPath()) ("Fallout4HDR-archive-" + [Guid]::NewGuid() + ".zip")
Add-Type -AssemblyName System.IO.Compression

function New-TestArchive {
    param([string[]]$ExtraEntries = @())

    $writer = [IO.Compression.ZipFile]::Open($archivePath, "Create")
    try {
        foreach ($name in @("Fallout4HDR/", "Fallout4HDR/src/example.cpp") + $ExtraEntries) {
            [void]$writer.CreateEntry($name)
        }
    }
    finally { $writer.Dispose() }
}

function Assert-ArchiveRejection {
    param([scriptblock]$Action, [string]$ExpectedMessage)

    try { & $Action }
    catch {
        if ($_.Exception.Message -notlike $ExpectedMessage) { throw }
        return
    }
    throw "Archive check did not reject the fixture: $ExpectedMessage"
}

try {
    foreach ($extra in @("", "unexpected.txt", "Fallout4HDR-other/file.txt", "other/")) {
        if ($extra) { New-TestArchive -ExtraEntries @($extra) }
        else { New-TestArchive }

        # Dependency archives still use the generic safe-path validator.
        $generic = Open-Fo4HdrValidatedZipArchive $archivePath
        $generic.Dispose()
        if (-not $extra) {
            $source = Open-Fo4HdrValidatedZipArchive $archivePath -RequiredRoot "Fallout4HDR"
            $source.Dispose()
        } else {
            Assert-ArchiveRejection {
                & (Join-Path $repoRoot "scripts/verify-source-package.ps1") -ArchivePath $archivePath
            } "Archive entry is outside the required Fallout4HDR/ root:*"
        }
        Remove-Item -LiteralPath $archivePath -Force
    }

    foreach ($case in @(
        @{ Entry = "/absolute.txt"; Error = "unsafe" },
        @{ Entry = "C:/absolute.txt"; Error = "unsafe" },
        @{ Entry = '\\server\share\absolute.txt'; Error = "unsafe" },
        @{ Entry = "Fallout4HDR/../outside.txt"; Error = "traversal" },
        @{ Entry = 'Fallout4HDR\..\outside.txt'; Error = "traversal" },
        @{ Entry = "Fallout4HDR/./src/other.cpp"; Error = "traversal" },
        @{ Entry = "Fallout4HDR/src/example.cpp"; Error = "duplicate" },
        @{ Entry = "Fallout4HDR/SRC/EXAMPLE.CPP"; Error = "duplicate" },
        @{ Entry = 'Fallout4HDR/src\example.cpp'; Error = "duplicate" }
    )) {
        New-TestArchive -ExtraEntries @($case.Entry)
        $expected = "Archive contains a " + $case.Error + " path:*"
        if ($case.Error -eq "unsafe") { $expected = "Archive contains an unsafe path:*" }
        Assert-ArchiveRejection {
            $archive = Open-Fo4HdrValidatedZipArchive $archivePath
            $archive.Dispose()
        } $expected
        if ($case.Entry -ceq "Fallout4HDR/../outside.txt") {
            Assert-ArchiveRejection {
                & (Join-Path $repoRoot "scripts/verify-source-package.ps1") -ArchivePath $archivePath
            } $expected
        }
        Remove-Item -LiteralPath $archivePath -Force
    }

    New-TestArchive -ExtraEntries @("unexpected.txt")
    $sidecarPath = $archivePath + ".sha256"
    $validHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    Set-Content -LiteralPath $sidecarPath -Value "$validHash  source.zip" -Encoding ascii
    Assert-ArchiveRejection {
        & (Join-Path $repoRoot "scripts/verify-source-package.ps1") -ArchivePath $archivePath
    } "Archive entry is outside the required Fallout4HDR/ root:*"
    Set-Content -LiteralPath $sidecarPath -Value (('0' * 64) + "  source.zip") -Encoding ascii
    Assert-ArchiveRejection {
        & (Join-Path $repoRoot "scripts/verify-source-package.ps1") -ArchivePath $archivePath
    } "Corresponding-source archive differs from its SHA-256 sidecar."
}
finally {
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
    if (Test-Path -LiteralPath ($archivePath + ".sha256")) {
        Remove-Item -LiteralPath ($archivePath + ".sha256") -Force
    }
}
Write-Host "Source archive root, path safety, duplicate and checksum validation passed."
