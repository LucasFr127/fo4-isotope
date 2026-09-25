# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")
. (Join-Path $repoRoot "scripts\internal\LicenseAuthorities.ps1")

$utf8 = [Text.UTF8Encoding]::new($false, $true)

$tempRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    ("Fallout4HDR-license-tests-" + [Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Path $tempRoot |
        Out-Null

    $path = Join-Path $tempRoot "MIT"
    $variants = @(
        ,$utf8.GetBytes("alpha`nbeta`n")
        ,$utf8.GetBytes("alpha`r`nbeta`r`n")
        ,([byte[]](0xEF, 0xBB, 0xBF) + $utf8.GetBytes("alpha`nbeta`n"))
        ,$utf8.GetBytes("alpha`nbeta")
    )

    $expectedHash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($utf8.GetBytes("alpha`nbeta`n"))
    ).ToLowerInvariant()
    foreach ($bytes in $variants) {
        [IO.File]::WriteAllBytes($path, [byte[]]$bytes)
        Assert-Fo4HdrLicenseTextHash $path $expectedHash "Equivalent license text was rejected."
    }

    [IO.File]::WriteAllBytes($path, $utf8.GetBytes("alpha`ngamma`n"))
    $changedRejected = $false
    try { Assert-Fo4HdrLicenseTextHash $path $expectedHash "Changed license text." }
    catch { $changedRejected = $_.Exception.Message -eq "Changed license text." }
    if (-not $changedRejected) { throw "License audit accepted changed license content." }

    [IO.File]::WriteAllBytes($path, [byte[]](0xC3, 0x28))
    $invalidRejected = $false
    try {
        Assert-Fo4HdrLicenseTextHash $path $expectedHash "Invalid license text."
    }
    catch {
        $invalidRejected = $_.Exception.Message -eq
            "Canonical text is not valid UTF-8: $path"
    }
    if (-not $invalidRejected) {
        throw "Invalid UTF-8 was accepted."
    }
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}

Write-Host "Canonical license regression tests passed."
