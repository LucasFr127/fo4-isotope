# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

function Copy-Profile {
    param([Parameter(Mandatory = $true)][psobject]$Profile)
    return $Profile | ConvertTo-Json -Depth 20 | ConvertFrom-Json
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Case
    )
    try {
        & $Action
    }
    catch {
        return
    }
    throw "Image-space profile regression was accepted: $Case"
}

$manifest = Get-Content (
    Join-Path $repoRoot "runtime\1.10.163\manifest.json"
) -Raw | ConvertFrom-Json
$profile = $manifest.commonlibObservation.imageSpaceProfile
$null = Assert-Fo4HdrImageSpaceProfile $profile

$wrongBoundary = Copy-Profile $profile
$wrongBoundary.topLevelEffectCount = 71
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $wrongBoundary } `
    "wrong top-level effect boundary"

$missing = Copy-Profile $profile
$missing.effects = @($missing.effects | Select-Object -First 71)
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $missing } `
    "missing effect"

$duplicate = Copy-Profile $profile
$duplicate.effects[71].symbol = $duplicate.effects[70].symbol
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $duplicate } `
    "duplicate symbol"

$wrongSymbol = Copy-Profile $profile
$wrongSymbol.effects[22].symbol = "EFFECT_NOT_THE_REVIEWED_MAP_SLOT"
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $wrongSymbol } `
    "wrong unique symbol"

$unordered = Copy-Profile $profile
$unordered.effects[1].index = 2
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $unordered } `
    "unordered index"

$wrongOffset = Copy-Profile $profile
$wrongOffset.activeData[4].offset = 148
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $wrongOffset } `
    "wrong active-data offset"

$wrongAlias = Copy-Profile $profile
$wrongAlias.activeData[1].xeditAlias = "Middle Gray"
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $wrongAlias } `
    "wrong xEdit field alias"

$legacyAliasSpelling = Copy-Profile $profile
$legacyAliasSpelling.activeData[1].xeditAlias = "tonemapE"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $legacyAliasSpelling
} "non-verbatim xEdit alias instead of its exact display label"

$missingAlias = Copy-Profile $profile
$missingAlias.activeData[1].PSObject.Properties.Remove("xeditAlias")
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $missingAlias } `
    "missing required xEdit field alias"

$unexpectedAlias = Copy-Profile $profile
$unexpectedAlias.activeData[0] | Add-Member `
    -NotePropertyName "xeditAlias" `
    -NotePropertyValue "Tonemap E"
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $unexpectedAlias } `
    "xEdit alias attached to the wrong CommonLib field"

$xeditNameAsCanonical = Copy-Profile $profile
$xeditNameAsCanonical.activeData[1].field = "Tonemap E"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $xeditNameAsCanonical
} "xEdit display label used as the canonical field name"

$wrongSource = Copy-Profile $profile
$wrongSource.sources.commonlib.commit = "0000000000000000000000000000000000000000"
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $wrongSource } `
    "wrong source identity"

$wrongRepository = Copy-Profile $profile
$wrongRepository.sources.commonlib.repository =
    "https://github.com/libxse/commonlibf4.git"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongRepository
} "obsolete CommonLibF4 repository"

$missingLayoutSource = Copy-Profile $profile
$missingLayoutSource.sources.commonlib.paths = @(
    $missingLayoutSource.sources.commonlib.paths |
        Where-Object { $_ -ne "include/RE/I/ImageSpaceBaseData.h" }
)
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $missingLayoutSource
} "missing CommonLib structural-layout source"

$wrongLayoutSource = Copy-Profile $profile
$wrongLayoutSource.sources.commonlib.paths[1] =
    "include/RE/I/ImageSpaceEffect.h"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongLayoutSource
} "wrong CommonLib structural-layout source"

$wrongAliasRepository = Copy-Profile $profile
$wrongAliasRepository.sources.xeditFieldAliases.repository =
    "https://example.invalid/xedit.git"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongAliasRepository
} "wrong xEdit alias repository"

$wrongAliasPath = Copy-Profile $profile
$wrongAliasPath.sources.xeditFieldAliases.path = "wrong.pas"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongAliasPath
} "wrong xEdit alias source path"

$wrongAliasCommit = Copy-Profile $profile
$wrongAliasCommit.sources.xeditFieldAliases.commit =
    "0000000000000000000000000000000000000000"
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongAliasCommit
} "wrong xEdit alias source commit"

$wrongManagerSize = Copy-Profile $profile
$wrongManagerSize.managerSize = 0x1B0
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $wrongManagerSize } `
    "wrong CommonLib manager size"

$wrongCurrentEofDataOffset = Copy-Profile $profile
$wrongCurrentEofDataOffset.currentEofDataOffset = 0x78
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongCurrentEofDataOffset
} "wrong CommonLib currentEOFData offset"

$missingLayout = Copy-Profile $profile
$missingLayout.PSObject.Properties.Remove("layout")
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $missingLayout } `
    "missing CommonLib structural layout"

$missingLayoutConstant = Copy-Profile $profile
$missingLayoutConstant.layout.manager.PSObject.Properties.Remove(
    "effectListOffset"
)
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $missingLayoutConstant
} "missing CommonLib structural-layout constant"

$wrongLayoutConstant = Copy-Profile $profile
$wrongLayoutConstant.layout.lut.textureOffset = 0x48
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongLayoutConstant
} "wrong CommonLib structural-layout constant"

$first = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    "Fallout4HDR.RuntimeIdentity.first.$([Guid]::NewGuid()).hpp")
$second = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    "Fallout4HDR.RuntimeIdentity.second.$([Guid]::NewGuid()).hpp")
try {
    & (Join-Path $repoRoot "scripts\generate-runtime-identity.ps1") `
        -Runtime "1.10.163" `
        -OutputPath $first
    & (Join-Path $repoRoot "scripts\generate-runtime-identity.ps1") `
        -Runtime "1.10.163" `
        -OutputPath $second
    if ((Get-FileHash -LiteralPath $first -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $second -Algorithm SHA256).Hash) {
        throw "Repeated runtime identity generation was not byte-identical."
    }

    [IO.File]::AppendAllText(
        $second,
        "// stale`n",
        [Text.UTF8Encoding]::new($false)
    )
    Assert-Rejected {
        & (Join-Path $repoRoot "scripts\generate-runtime-identity.ps1") `
            -Runtime "1.10.163" `
            -OutputPath $second `
            -Check
    } "stale generated output"
}
finally {
    Remove-Item -LiteralPath $first, $second -Force -ErrorAction SilentlyContinue
}

Write-Host "Image-space semantic profile regression tests passed."
