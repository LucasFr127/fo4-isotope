# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$DisablePch,

    [switch]$DisableFrameTrace
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

& (Join-Path $PSScriptRoot "configure.ps1") `
    -Mode $Mode `
    -DisablePch:$DisablePch `
    -DisableFrameTrace:$DisableFrameTrace

$repoRoot = Get-Fo4HdrRepoRoot
Push-Location $repoRoot
try {
    xmake build -v
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }

    # The stage holds exactly this build's files; xmake install never deletes
    # files an earlier build staged, such as an old INI.
    $stage = Join-Path $repoRoot "dist\universal"
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
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
    -Mode $Mode
