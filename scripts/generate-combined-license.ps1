# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$OutputPath,

    [switch]$Check
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
if (-not $OutputPath) {
    $OutputPath = Join-Path `
        $repoRoot `
        "LICENSES\LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions.txt"
}

$gpl = Read-Fo4HdrCanonicalUtf8Text (
    Join-Path $repoRoot "COPYING.GPL-3.0-or-later"
)
$exceptions = Read-Fo4HdrCanonicalUtf8Text (
    Join-Path $repoRoot "COPYING.CommonLib-Exceptions"
)

$introLines = @(
    "FO4Isotope combined licensing terms",
    "====================================",
    "",
    "SPDX custom license identifier:",
    "LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions",
    "",
    "Project-owned FO4Isotope source is licensed under GNU GPL version 3 or any",
    "later version, with the additional permissions reproduced after the GPL text.",
    "The additional-permission text is kept verbatim from the pinned",
    'Dear-Modding-FO4 `commonlib-shared/EXCEPTIONS` file used by the project.',
    "",
    "GNU GENERAL PUBLIC LICENSE",
    "--------------------------",
    ""
)
$permissionLines = @(
    "",
    "ADDITIONAL PERMISSIONS",
    "----------------------",
    ""
)

$combined =
    (($introLines -join "`n") + "`n") +
    $gpl +
    (($permissionLines -join "`n") + "`n") +
    $exceptions

if ($Check) {
    if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        throw "Combined LicenseRef file does not exist: $OutputPath"
    }

    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $expected = $utf8.GetBytes($combined)
    $actual = [IO.File]::ReadAllBytes($OutputPath)
    if (-not [Linq.Enumerable]::SequenceEqual[byte](
        $actual,
        $expected
    )) {
        throw (
            "Combined LicenseRef is not the deterministic output of " +
            "the canonical license inputs."
        )
    }

    Write-Host "Combined LicenseRef is canonical and deterministic."
    return
}

Write-Fo4HdrCanonicalUtf8Text `
    -Path $OutputPath `
    -Text $combined
Write-Host "Generated combined LicenseRef: $OutputPath"
