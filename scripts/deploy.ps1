# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = "High")]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime = $env:FO4HDR_RUNTIME,

    [switch]$AllowUnconventionalDestination
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

& (Join-Path $PSScriptRoot "validate-runtime.ps1") `
    -Runtime $Runtime

if (-not $env:FO4_MOD_ROOT) {
    throw "FO4_MOD_ROOT is not set."
}

$repoRoot = Get-Fo4HdrRepoRoot
$stageRoot = Join-Path $repoRoot "dist"

$source = Join-Path $stageRoot "$Runtime\Data"
$destination = $env:FO4_MOD_ROOT

if (-not (Test-Path $source)) {
    throw "Staged runtime is absent: $source"
}

& (Join-Path $PSScriptRoot "verify-plugin-metadata.ps1") `
    -Runtime $Runtime `
    -StageOnly

$expectedRelativeFiles = @(
    "F4SE\Plugins\Fallout4HDR.dll",
    "F4SE\Plugins\Fallout4HDR.pdb",
    "F4SE\Plugins\Fallout4HDR.ini",
    "F4SE\Plugins\Fallout4HDR\Default.ini",
    "F4SE\Plugins\Fallout4HDR\runtime-$Runtime.json"
)
$actualRelativeFiles = @(
    Get-ChildItem $source -Recurse -File |
        ForEach-Object {
            [IO.Path]::GetRelativePath($source, $_.FullName)
        } |
        Sort-Object
)
$unexpected = @($actualRelativeFiles | Where-Object {
    $_ -notin $expectedRelativeFiles
})
$missing = @($expectedRelativeFiles | Where-Object {
    $_ -notin $actualRelativeFiles
})
if ($unexpected.Count -ne 0 -or $missing.Count -ne 0) {
    throw (
        "Staged runtime file set is not exact. Missing: " +
        ($missing -join ", ") + "; unexpected: " +
        ($unexpected -join ", ")
    )
}

if (Test-IsSameOrChildPath $destination $repoRoot) {
    throw "Deployment destination must not be inside the repository."
}

if ($env:FO4_ROOT -and
    (Test-IsSameOrChildPath $destination $env:FO4_ROOT)) {
    throw "Deployment destination must not be inside FO4_ROOT."
}

$destinationLeaf = Split-Path $destination -Leaf
if (-not $AllowUnconventionalDestination -and
    $destinationLeaf -notmatch 'Fallout4HDR') {
    throw (
        "FO4_MOD_ROOT does not look like a dedicated Fallout4HDR mod. " +
        "Use a dedicated mod directory or pass " +
        "-AllowUnconventionalDestination deliberately."
    )
}

$sourceFiles = Get-ChildItem $source -Recurse -File |
    Sort-Object FullName

$newRelativeFiles = @(
    $sourceFiles |
    ForEach-Object {
        [IO.Path]::GetRelativePath($source, $_.FullName)
    }
)

$ownedManifestPath =
    Join-Path $destination ".fallout4hdr-owned-files.json"
$oldRelativeFiles = @()

if (Test-Path $ownedManifestPath) {
    $oldManifest =
        Get-Content $ownedManifestPath -Raw |
        ConvertFrom-Json
    $oldRelativeFiles = @($oldManifest.files)
}

$obsolete = @(
    $oldRelativeFiles |
    Where-Object {
        $_ -notin $newRelativeFiles -and
        ([string]$_).Replace('/', '\') -ine
            "F4SE\Plugins\Fallout4HDR\Personal.ini"
    }
)

if ($PSCmdlet.ShouldProcess(
    $destination,
    "Deploy Fallout4HDR $Runtime"
)) {
    New-Item -ItemType Directory -Force $destination |
        Out-Null

    foreach ($relative in $obsolete) {
        $obsoletePath = Resolve-Fo4HdrChildPath `
            -Parent $destination `
            -RelativePath ([string]$relative)
        if (Test-Path $obsoletePath -PathType Leaf) {
            Remove-Item $obsoletePath -Force
        }
    }

    foreach ($file in $sourceFiles) {
        $relative =
            [IO.Path]::GetRelativePath($source, $file.FullName)
        $target = Resolve-Fo4HdrChildPath `
            -Parent $destination `
            -RelativePath $relative
        $targetParent = Split-Path -Parent $target
        New-Item -ItemType Directory -Force $targetParent |
            Out-Null
        Copy-Item $file.FullName $target -Force
    }

    [ordered]@{
        schemaVersion = 1
        runtime = $Runtime
        deployedAtUtc = [DateTime]::UtcNow.ToString("o")
        files = $newRelativeFiles
    } | ConvertTo-Json -Depth 8 |
        Set-Content $ownedManifestPath -Encoding utf8NoBOM

    Write-Host "Deployed to dedicated MO2 mod: $destination"
}
