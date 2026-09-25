# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime = $env:FO4HDR_RUNTIME,

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$AllowUnvalidatedRuntime,

    [switch]$DisablePch
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

if (-not $Runtime) {
    throw "Specify -Runtime or set FO4HDR_RUNTIME."
}

$repoRoot = Get-Fo4HdrRepoRoot
& (Join-Path $PSScriptRoot "validate-manifests.ps1")
if ($Runtime -eq "1.10.163") {
    & (Join-Path $PSScriptRoot "generate-runtime-identity.ps1") `
        -Runtime $Runtime `
        -Check
}

$manifestPath =
    Join-Path $repoRoot "runtime\$Runtime\manifest.json"
$manifest =
    Get-Content $manifestPath -Raw | ConvertFrom-Json
$status = [string]$manifest.runtime.status
$lock = Get-Content (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
$lockedStatus = [string]$lock.runtimes.$Runtime.status
if ($status -ne $lockedStatus) {
    throw "Runtime manifest and versions.lock.json disagree on status."
}
Assert-Fo4HdrRuntimeActionAllowed `
    -Status $lockedStatus `
    -Action "developer-build" `
    -AllowUnvalidatedRuntime:$AllowUnvalidatedRuntime

$allowFlag = if ($AllowUnvalidatedRuntime) { "y" } else { "n" }
$pchFlag = if ($DisablePch) { "n" } else { "y" }
$toolset = Get-Fo4HdrMsvcToolset -Lock $lock
$sdk = [string]$lock.toolchain.observed.windowsSdk.version

Push-Location $repoRoot
try {
    xmake f `
        -y `
        -p windows `
        -a x64 `
        --toolchain=msvc `
        --vs_toolset=$toolset `
        --vs_sdkver=$sdk `
        -m $Mode `
        --fo4hdr_runtime=$Runtime `
        --fo4hdr_allow_unvalidated_runtime=$allowFlag `
        --fo4hdr_use_pch=$pchFlag `
        --fo4hdr_build_tests=y `
        --commonlib_ini=n `
        --commonlib_json=n `
        --commonlib_toml=n `
        --commonlib_xbyak=n

    if ($LASTEXITCODE -ne 0) {
        throw "XMake configure failed."
    }
    Assert-Fo4HdrConfiguredMsvc -Lock $lock
}
finally {
    Pop-Location
}

Write-Host "Configured $Runtime for Windows x64 in $Mode mode."
