# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.240")]
    [string]$Runtime = $env:FO4HDR_RUNTIME
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

if (-not $Runtime) {
    throw "Specify -Runtime or set FO4HDR_RUNTIME."
}
if (-not $env:FO4_ROOT) {
    throw "FO4_ROOT is not set."
}

$repoRoot = Get-Fo4HdrRepoRoot
& (Join-Path $PSScriptRoot "validate-manifests.ps1")

$lock = Get-Content (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
$entry = $lock.runtimes.$Runtime
$definition = Get-Fo4HdrRuntimeDefinition $Runtime

$manifestPath =
    Join-Path $repoRoot "runtime\$Runtime\manifest.json"
$manifest =
    Get-Content $manifestPath -Raw | ConvertFrom-Json

Assert-Fo4HdrRuntimeActionAllowed `
    -Status ([string]$entry.status) `
    -Action "runtime-identity-validation"

$paths = @{
    exe = Join-Path $env:FO4_ROOT "Fallout4.exe"
    loader = Join-Path $env:FO4_ROOT "f4se_loader.exe"
    runtime = Join-Path $env:FO4_ROOT $definition.RuntimeDll
    address = Get-Fo4AddressLibraryPath `
        $definition.AddressLibraryFile
}

foreach ($item in $paths.GetEnumerator()) {
    if (-not (Test-Path $item.Value)) {
        throw "Missing $($item.Key): $($item.Value)"
    }
}

function Require-Hash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Expected,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($Expected -notmatch '^[0-9a-fA-F]{64}$') {
        throw "$Name hash is uninitialized."
    }

    $actual = Get-Sha256 $Path
    if ($actual -ne $Expected.ToLowerInvariant()) {
        throw "$Name hash mismatch. Expected $Expected; got $actual"
    }
}

$fileVersion =
    [Diagnostics.FileVersionInfo]::GetVersionInfo(
        $paths.exe
    ).FileVersion

Assert-Fo4HdrExactRuntimeFileVersion `
    -Runtime $Runtime `
    -FileVersion $fileVersion

if ($Runtime -in @("1.10.163", "1.10.984", "1.11.240")) {
    & (Join-Path $PSScriptRoot "generate-runtime-identity.ps1") `
        -Runtime $Runtime `
        -Check
}

Require-Hash $paths.exe $entry.exeSha256 "Fallout4.exe"
Require-Hash $paths.loader $entry.f4seLoaderSha256 "f4se_loader.exe"
Require-Hash `
    $paths.runtime `
    $entry.f4seRuntimeDllSha256 `
    $definition.RuntimeDll
Require-Hash `
    $paths.address `
    $entry.addressLibrarySha256 `
    "Address Library"

$requiredRelocations = @()
if ($null -ne $manifest.PSObject.Properties['commonlibObservation']) {
    $requiredRelocations = @($manifest.commonlibObservation.relocations)
}
$addressLibrary = Assert-Fo4HdrAddressLibraryV0 `
    -Path $paths.address `
    -ExecutablePath $paths.exe `
    -RequiredRelocations $requiredRelocations

Write-Host (
    "Runtime identity validation passed for $Runtime " +
    "with status '$($entry.status)'."
)
Write-Host "Game root:       $($paths.exe | Split-Path -Parent)"
Write-Host "Address Library: $($paths.address)"
Write-Host (
    "Address IDs:     {0} entries; {1} required IDs validated" -f
    $addressLibrary.Count,
    $addressLibrary.RequiredCount
)
