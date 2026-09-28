# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

function Copy-Observation {
    param([Parameter(Mandatory = $true)][psobject]$Observation)
    return $Observation | ConvertTo-Json -Depth 30 | ConvertFrom-Json
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
    throw "CommonLib relocation profile regression was accepted: $Case"
}

$manifest = Get-Content (
    Join-Path $repoRoot "runtime\1.10.163\manifest.json"
) -Raw | ConvertFrom-Json
$lock = Get-Content (Join-Path $repoRoot "versions.lock.json") `
    -Raw | ConvertFrom-Json
$observation = $manifest.commonlibObservation
$null = Assert-Fo4HdrCommonLibRelocationProfile `
    -Observation $observation `
    -CommonLibLock $lock.commonlib

$missing = Copy-Observation $observation
$missing.relocations = @($missing.relocations | Select-Object -First 7)
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile $missing $lock.commonlib
} "missing row"

$extra = Copy-Observation $observation
$extra.relocations = @($extra.relocations) + @($extra.relocations[7])
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile $extra $lock.commonlib
} "extra row"

$missingSymbol = Copy-Observation $observation
$missingSymbol.relocations[0].PSObject.Properties.Remove("commonlibSymbol")
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile `
        $missingSymbol $lock.commonlib
} "missing symbol"

$swappedSymbols = Copy-Observation $observation
$firstSymbol = $swappedSymbols.relocations[0].commonlibSymbol
$swappedSymbols.relocations[0].commonlibSymbol =
    $swappedSymbols.relocations[1].commonlibSymbol
$swappedSymbols.relocations[1].commonlibSymbol = $firstSymbol
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile `
        $swappedSymbols $lock.commonlib
} "unique symbols assigned to the wrong providers"

$duplicateProvider = Copy-Observation $observation
$duplicateProvider.relocations[1].provider =
    $duplicateProvider.relocations[0].provider
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile `
        $duplicateProvider $lock.commonlib
} "duplicate provider"

$duplicateSymbol = Copy-Observation $observation
$duplicateSymbol.relocations[1].commonlibSymbol =
    $duplicateSymbol.relocations[0].commonlibSymbol
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile `
        $duplicateSymbol $lock.commonlib
} "duplicate symbol"

$wrongKind = Copy-Observation $observation
$wrongKind.relocations[0].kind = "function"
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile $wrongKind $lock.commonlib
} "wrong provider kind"

$duplicateId = Copy-Observation $observation
$duplicateId.relocations[1].id = $duplicateId.relocations[0].id
Assert-Rejected {
    $null = Assert-Fo4HdrCommonLibRelocationProfile $duplicateId $lock.commonlib
} "duplicate ID"

foreach ($property in @("id", "rva")) {
    $zero = Copy-Observation $observation
    $zero.relocations[0].$property = 0
    Assert-Rejected {
        $null = Assert-Fo4HdrCommonLibRelocationProfile `
            $zero $lock.commonlib
    } "zero $property"
}

foreach ($sourceMutation in @(
    @{ Property = "repository"; Value = "https://github.com/libxse/commonlibf4.git" },
    @{ Property = "commit"; Value = "0000000000000000000000000000000000000000" },
    @{ Property = "path"; Value = "include/RE/Relocation.h" }
)) {
    $wrongSource = Copy-Observation $observation
    $wrongSource.relocationSource.($sourceMutation.Property) =
        $sourceMutation.Value
    Assert-Rejected {
        $null = Assert-Fo4HdrCommonLibRelocationProfile `
            $wrongSource $lock.commonlib
    } "wrong relocation source $($sourceMutation.Property)"
}

$generated = Get-Content (
    Join-Path $repoRoot "runtime\1.10.163\RuntimeIdentity.generated.hpp"
) -Raw
foreach ($retiredPattern in @(
    'CommonLibRelocationIdentity',
    'k[A-Za-z]+RelocationId',
    'k[A-Za-z]+RelocationRva'
)) {
    if ($generated -match $retiredPattern) {
        throw "Generated production relocation table was not retired: $retiredPattern"
    }
}

& (Join-Path $repoRoot "scripts\generate-runtime-identity.ps1") `
    -Runtime "1.10.163" `
    -Check

Write-Host "CommonLib relocation profile regression tests passed."
