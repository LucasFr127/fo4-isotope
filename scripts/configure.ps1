# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$AllowUnvalidatedRuntime,

    [switch]$DisablePch,

    [switch]$DisableFrameTrace
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
& (Join-Path $PSScriptRoot "validate-manifests.ps1")
$lock = Get-Content (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
# Every runtime profile is compiled into the one universal DLL.
foreach ($profileRuntime in @("1.10.163", "1.10.984", "1.11.240")) {
    & (Join-Path $PSScriptRoot "generate-runtime-identity.ps1") `
        -Runtime $profileRuntime `
        -Check

    $manifestPath =
        Join-Path $repoRoot "runtime\$profileRuntime\manifest.json"
    $manifest =
        Get-Content $manifestPath -Raw | ConvertFrom-Json
    $status = [string]$manifest.runtime.status
    $lockedStatus = [string]$lock.runtimes.$profileRuntime.status
    if ($status -ne $lockedStatus) {
        throw "Runtime $profileRuntime manifest and versions.lock.json disagree on status."
    }
    Assert-Fo4HdrRuntimeActionAllowed `
        -Status $lockedStatus `
        -Action "developer-build" `
        -AllowUnvalidatedRuntime:$AllowUnvalidatedRuntime
}

$allowFlag = if ($AllowUnvalidatedRuntime) { "y" } else { "n" }
$pchFlag = if ($DisablePch) { "n" } else { "y" }
$traceFlag = if ($DisableFrameTrace) { "n" } else { "y" }
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
        --fo4hdr_allow_unvalidated_runtime=$allowFlag `
        --fo4hdr_use_pch=$pchFlag `
        --fo4hdr_frame_trace=$traceFlag `
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

Write-Host "Configured the universal OG/NG/AE build for Windows x64 in $Mode mode."
