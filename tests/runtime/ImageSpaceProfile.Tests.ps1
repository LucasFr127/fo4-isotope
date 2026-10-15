# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
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

$manifestSchema = Join-Path $repoRoot 'schemas/runtime-manifest.schema.json'
if (-not ($manifest | ConvertTo-Json -Depth 30 | Test-Json -SchemaFile $manifestSchema)) {
    throw 'The runtime manifest without xEdit source metadata is invalid.'
}
foreach ($case in @('sunlightColorIndex', 'glareColorIndex', 'rgbByteShifts')) {
    $invalidWeather = Copy-Profile $manifest
    if ($case -eq 'rgbByteShifts') {
        $invalidWeather.imageSpaceObserver.weatherGrade.$case = @(16, 8, 0)
    } else {
        $invalidWeather.imageSpaceObserver.weatherGrade.$case = 0
    }
    Assert-Rejected {
        $null = $invalidWeather | ConvertTo-Json -Depth 30 |
            Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
    } "unqualified weather color mapping: $case"
}

$retiredWeatherFields = [ordered]@{
    chronologicalSlots = @(4, 0, 5, 1, 6, 2, 7, 3)
    source = @{
        repository = 'https://github.com/TES5Edit/TES5Edit.git'
        commit = 'ca51ca1b67c2e84965c56d9163cec2acf20d6722'
        # Preserve the former path so this fixture remains valid under the retired schema.
        path = 'Core/wbDefinitionsFO4.pas'
    }
}
foreach ($case in $retiredWeatherFields.Keys) {
    $retiredWeather = Copy-Profile $manifest
    $retiredWeather.imageSpaceObserver.weatherGrade |
        Add-Member -NotePropertyName $case -NotePropertyValue $retiredWeatherFields[$case]
    Assert-Rejected {
        $null = $retiredWeather | ConvertTo-Json -Depth 30 |
            Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
    } "retired weather metadata: $case"
}

$retiredAliasSource = Copy-Profile $manifest
$retiredAliasSource.commonlibObservation.imageSpaceProfile.sources |
    Add-Member -NotePropertyName 'xeditFieldAliases' -NotePropertyValue @{
        repository = 'https://github.com/TES5Edit/TES5Edit.git'
        commit = 'ca51ca1b67c2e84965c56d9163cec2acf20d6722'
        path = 'wbDefinitionsFO4.pas'
    }
Assert-Rejected {
    $null = $retiredAliasSource | ConvertTo-Json -Depth 30 |
        Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
} 'retired xEdit source metadata'

foreach ($case in @('managerSize', 'currentEofDataOffset', 'topLevelEffectCount', 'effects')) {
    $retired = Copy-Profile $manifest
    $retired.commonlibObservation.imageSpaceProfile |
        Add-Member -NotePropertyName $case -NotePropertyValue 0
    Assert-Rejected {
        $null = $retired | ConvertTo-Json -Depth 30 |
            Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
    } "duplicated CommonLib profile fact: $case"
}
foreach ($case in @('manager', 'effect', 'lut')) {
    $retired = Copy-Profile $manifest
    $retired.commonlibObservation.imageSpaceProfile.layout |
        Add-Member -NotePropertyName $case -NotePropertyValue @{}
    Assert-Rejected {
        $null = $retired | ConvertTo-Json -Depth 30 |
            Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
    } "duplicated CommonLib public layout: $case"
}

$retiredOffset = Copy-Profile $manifest
$retiredOffset.commonlibObservation.imageSpaceProfile.activeData[0] |
    Add-Member -NotePropertyName 'offset' -NotePropertyValue 128
Assert-Rejected {
    $null = $retiredOffset | ConvertTo-Json -Depth 30 |
        Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
} 'duplicated CommonLib active-data offset'

$retiredTextureOffset = Copy-Profile $manifest
$retiredTextureOffset.nativeLutObserver |
    Add-Member -NotePropertyName 'textureSrvOffset' -NotePropertyValue 0
Assert-Rejected {
    $null = $retiredTextureOffset | ConvertTo-Json -Depth 30 |
        Test-Json -SchemaFile $manifestSchema -ErrorAction Stop
} 'duplicated CommonLib texture SRV offset'

$missing = Copy-Profile $profile
$missing.activeData = @($missing.activeData | Select-Object -First 11)
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $missing } `
    "missing active-data field"

$duplicate = Copy-Profile $profile
$duplicate.activeData[11].field = $duplicate.activeData[10].field
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $duplicate } `
    "duplicate active-data field"

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

$missingLayout = Copy-Profile $profile
$missingLayout.PSObject.Properties.Remove("layout")
Assert-Rejected { $null = Assert-Fo4HdrImageSpaceProfile $missingLayout } `
    "missing private array layout"

$missingLayoutConstant = Copy-Profile $profile
$missingLayoutConstant.layout.primitiveArray.PSObject.Properties.Remove(
    "freeIndexOffset"
)
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $missingLayoutConstant
} "missing private array-layout constant"

$wrongLayoutConstant = Copy-Profile $profile
$wrongLayoutConstant.layout.primitiveArray.freeIndexOffset = 0x10
Assert-Rejected {
    $null = Assert-Fo4HdrImageSpaceProfile $wrongLayoutConstant
} "wrong private array-layout constant"

$first = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    "FO4Isotope.RuntimeIdentity.first.$([Guid]::NewGuid()).hpp")
$second = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    "FO4Isotope.RuntimeIdentity.second.$([Guid]::NewGuid()).hpp")
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
