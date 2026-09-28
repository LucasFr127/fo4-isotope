# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$Repository =
        "https://github.com/Dear-Modding-FO4/commonlibf4.git"
)

$ErrorActionPreference = "Stop"

if (-not (
    Get-Command git -ErrorAction SilentlyContinue
)) {
    throw "Git is required."
}

$line = git ls-remote $Repository refs/heads/main
if ($LASTEXITCODE -ne 0 -or -not $line) {
    throw "Unable to query CommonLibF4 main."
}

$parts = $line -split '\s+'
$commit = $parts[0].ToLowerInvariant()
if ($commit -notmatch '^[0-9a-f]{40}$') {
    throw "Unexpected commit returned by git ls-remote."
}

Write-Host "Current CommonLibF4 main commit:"
Write-Host $commit
Write-Host ""
Write-Host (
    "Review this exact commit before passing it to " +
    "scripts/add-commonlib.ps1. Querying does not approve or pin it."
)
