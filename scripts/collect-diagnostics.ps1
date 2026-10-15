# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.240")]
    [string]$Runtime = $env:FO4HDR_RUNTIME
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

if (-not $env:FO4_CAPTURE_ROOT) {
    throw "FO4_CAPTURE_ROOT is not set."
}
if (-not $Runtime) {
    throw "Specify -Runtime or set FO4HDR_RUNTIME."
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$destination =
    Join-Path $env:FO4_CAPTURE_ROOT "diagnostics\$timestamp"
New-Item -ItemType Directory -Force $destination |
    Out-Null

if ($env:FO4_LOG_ROOT -and
    (Test-Path $env:FO4_LOG_ROOT)) {
    Copy-Item `
        -Path (Join-Path $env:FO4_LOG_ROOT "*") `
        -Destination $destination `
        -Recurse `
        -Force
}

$repoRoot = Get-Fo4HdrRepoRoot
Copy-Item `
    (Join-Path $repoRoot "versions.lock.json") `
    $destination `
    -Force
Copy-Item `
    (Join-Path $repoRoot "runtime\$Runtime\manifest.json") `
    $destination `
    -Force

Get-ActiveToolchainInfo |
    ConvertTo-Json -Depth 8 |
    Set-Content `
        (Join-Path $destination "active-toolchain.json") `
        -Encoding utf8NoBOM

git -C $repoRoot status --short |
    Set-Content (
        Join-Path $destination "git-status.txt"
    )

git -C $repoRoot submodule status --recursive |
    Set-Content (
        Join-Path $destination "submodules.txt"
    )

Write-Host "Diagnostics collected at: $destination"
