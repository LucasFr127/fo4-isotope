# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime = $env:FO4HDR_RUNTIME,

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$AllowUnvalidatedRuntime,

    [switch]$DisablePch,

    [switch]$DisableFrameTrace
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

& (Join-Path $PSScriptRoot "configure.ps1") `
    -Runtime $Runtime `
    -Mode $Mode `
    -AllowUnvalidatedRuntime:$AllowUnvalidatedRuntime `
    -DisablePch:$DisablePch `
    -DisableFrameTrace:$DisableFrameTrace

$repoRoot = Get-Fo4HdrRepoRoot
Push-Location $repoRoot
try {
    xmake build -v
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }

    $retiredDefault = Resolve-Fo4HdrChildPath `
        -Parent (Join-Path $repoRoot "dist") `
        -RelativePath "$Runtime/Data/F4SE/Plugins/FO4Isotope/Default.ini"
    if (Test-Path -LiteralPath $retiredDefault -PathType Leaf) {
        Remove-Item -LiteralPath $retiredDefault -Force
    }

    xmake install -v FO4Isotope
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin staging failed."
    }
}
finally {
    Pop-Location
}

& (Join-Path $PSScriptRoot "verify-plugin-metadata.ps1") `
    -Runtime $Runtime `
    -Mode $Mode
