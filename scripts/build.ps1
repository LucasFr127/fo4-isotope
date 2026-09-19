# SPDX-License-Identifier: GPL-3.0-or-later
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

& (Join-Path $PSScriptRoot "configure.ps1") `
    -Runtime $Runtime `
    -Mode $Mode `
    -AllowUnvalidatedRuntime:$AllowUnvalidatedRuntime `
    -DisablePch:$DisablePch

$repoRoot = Get-Fo4HdrRepoRoot
Push-Location $repoRoot
try {
    xmake build -v
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }

    xmake install -v Fallout4HDR
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin staging failed."
    }
}
finally {
    Pop-Location
}

if ($Runtime -eq "1.10.163") {
    & (Join-Path $PSScriptRoot "verify-plugin-metadata.ps1") `
        -Runtime $Runtime `
        -Mode $Mode
}
