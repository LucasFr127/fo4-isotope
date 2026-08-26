# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

$ErrorActionPreference = 'Stop'
. ([IO.Path]::Combine($PSScriptRoot, '../../scripts/internal/Project.Source.ps1'))

function Test-ArchiveNames {
    param([string]$Path, [string[]]$Names, [bool]$Expected)

    $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $Names) { [void]$archive.CreateEntry($name) }
    }
    finally { $archive.Dispose() }
    $accepted = $false
    try {
        $validated = Open-Fo4HdrValidatedZipArchive $Path
        try { $accepted = $true }
        finally { $validated.Dispose() }
    }
    catch { $accepted = $false }
    if ($accepted -ne $Expected) { throw "Unexpected archive name admission: $($Names -join ', ')" }
}

$temp = [IO.Path]::Combine([IO.Path]::GetTempPath(), "Fallout4HDR-archive-$([Guid]::NewGuid())")
[void][IO.Directory]::CreateDirectory($temp)
try {
    $cases = @(
        @{ Names = @('src/main.cpp', 'COPYING.GPL-3.0-or-later'); Accepted = $true },
        @{ Names = @('../escape'); Accepted = $false },
        @{ Names = @('/absolute'); Accepted = $false },
        @{ Names = @('C:/absolute'); Accepted = $false },
        @{ Names = @('src\alias'); Accepted = $false },
        @{ Names = @('src//empty'); Accepted = $false },
        @{ Names = @('src/./alias'); Accepted = $false },
        @{ Names = @('src/name.'); Accepted = $false },
        @{ Names = @('src/NUL.txt'); Accepted = $false },
        @{ Names = @('src/main.cpp', 'SRC/MAIN.cpp'); Accepted = $false },
        @{ Names = @('src', 'src/'); Accepted = $false }
    )
    $index = 0
    foreach ($case in $cases) {
        Test-ArchiveNames ([IO.Path]::Combine($temp, "$index.zip")) $case.Names $case.Accepted
        ++$index
    }
    Write-Host 'PASS archive path and Windows name admission'
}
finally { [IO.Directory]::Delete($temp, $true) }
