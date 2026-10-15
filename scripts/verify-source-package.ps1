# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,

    [ValidateSet("1.10.163", "1.10.984", "1.11.240")]
    [string]$Runtime = "1.10.163",

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$KeepExtracted
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$archive = [IO.Path]::GetFullPath($ArchivePath)
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    throw "Corresponding-source archive does not exist: $archive"
}

$sidecar = $archive + ".sha256"
if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
    $expectedHash = (
        Get-Content -LiteralPath $sidecar -Raw
    ).Split([char[]]" `t`r`n", [StringSplitOptions]::RemoveEmptyEntries)[0]
    if ((Get-Sha256 $archive) -ne $expectedHash.ToLowerInvariant()) {
        throw "Corresponding-source archive differs from its SHA-256 sidecar."
    }
}

$validatedArchive = Open-Fo4HdrValidatedZipArchive $archive -RequiredRoot "FO4Isotope"
$validatedArchive.Dispose()

$extractRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    ("F4H Src " + [Guid]::NewGuid().ToString("N").Substring(0, 8))
$projectRoot = Join-Path $extractRoot "FO4Isotope"
$environmentNames = @(
    "FO4_ROOT",
    "FO4_MOD_ROOT",
    "FO4_ADDRESS_LIBRARY_FILE",
    "XSE_FO4_GAME_PATH",
    "XSE_FO4_MODS_PATH"
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] =
        [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
    New-Item -ItemType Directory -Force $extractRoot | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory(
        $archive,
        $extractRoot
    )

    if (-not (Test-Path -LiteralPath $projectRoot -PathType Container)) {
        throw "Archive does not contain the stable FO4Isotope top-level directory."
    }

    & (Join-Path $projectRoot "scripts\license-audit.ps1") `
        -PackageRoot $projectRoot `
        -Scope full

    foreach ($name in $environmentNames) {
        Remove-Item "Env:\$name" -ErrorAction SilentlyContinue
    }

    Push-Location $projectRoot
    try {
        $powerShell = Get-Fo4HdrPowerShellPath
        & $powerShell `
            -NoProfile `
            -File (Join-Path $projectRoot "scripts\build.ps1") `
            -Runtime $Runtime `
            -Mode $Mode
        if ($LASTEXITCODE -ne 0) {
            throw "Extracted source-package build failed."
        }

        & $powerShell `
            -NoProfile `
            -File (Join-Path $projectRoot "scripts\test.ps1") `
            -Runtime $Runtime `
            -Mode $Mode `
            -SkipConfigure
        if ($LASTEXITCODE -ne 0) {
            throw "Extracted source-package tests failed."
        }
    }
    finally {
        Pop-Location
    }

    Write-Host "Clean-directory source rebuild passed: $projectRoot"
    Write-Host "Dependency resolution used the configured XMake caches and network; this is not an offline rebuild."
}
finally {
    if (-not $KeepExtracted -and
        (Test-Path -LiteralPath $extractRoot)) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name,
            $savedEnvironment[$name],
            "Process"
        )
    }
}
