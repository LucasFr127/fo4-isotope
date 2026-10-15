# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

Set-StrictMode -Version Latest

function Test-Fo4HdrRelocationSourceIdentity {
    param($Source, $CommonLibLock)
    return -not ($null -eq $source -or
        @($source.PSObject.Properties.Name).Count -ne 3 -or
        [string]$source.repository -ne [string]$CommonLibLock.repository -or
        [string]$source.commit -ne [string]$CommonLibLock.commit -or
        [string]$source.path -ne "include/RE/IDs.h")
}

function Assert-Fo4HdrRelocationSourceIdentity {
    param($Observation, $CommonLibLock)
    $source = $Observation.relocationSource
    if (-not (Test-Fo4HdrRelocationSourceIdentity $source $CommonLibLock)) {
        throw "CommonLib relocation source identity is invalid."
    }

}

function Get-Fo4HdrRequiredRelocationProfile {
    return @(
        [pscustomobject]@{
            Provider = "image-space-manager"
            Kind = "pointer-storage"
            Symbol = "RE::ID::ImageSpaceManager::Singleton"
        },
        [pscustomobject]@{
            Provider = "renderer-data"
            Kind = "pointer-storage"
            Symbol = "RE::ID::BSGraphics::GetRendererData"
        },
        [pscustomobject]@{
            Provider = "render-target-manager"
            Kind = "direct-object"
            Symbol = "RE::ID::BSGraphics::RenderTargetManager::Singleton"
        },
        [pscustomobject]@{
            Provider = "ui"
            Kind = "pointer-storage"
            Symbol = "RE::ID::UI::Singleton"
        },
        [pscustomobject]@{
            Provider = "ui-event-spin-lock"
            Kind = "function"
            Symbol = "RE::ID::BSSpinLock::lock"
        },
        [pscustomobject]@{
            Provider = "ui-event-memory-manager"
            Kind = "function"
            Symbol = "RE::ID::MemoryManager::Singleton"
        },
        [pscustomobject]@{
            Provider = "ui-event-memory-allocate"
            Kind = "function"
            Symbol = "RE::ID::MemoryManager::Allocate"
        },
        [pscustomobject]@{
            Provider = "ui-event-memory-deallocate"
            Kind = "function"
            Symbol = "RE::ID::MemoryManager::Deallocate"
        }
    )
}

function Test-Fo4HdrRelocationRowIdentity {
    param($Row, $Identity)
    return -not (@($row.PSObject.Properties.Name).Count -ne 5 -or
        [string]$row.provider -ne $identity.Provider -or
        [string]$row.kind -ne $identity.Kind -or
        [string]$row.commonlibSymbol -ne $identity.Symbol)
}

function Assert-Fo4HdrRelocationRowIdentity {
    param($Row, $Identity)
    if (-not (Test-Fo4HdrRelocationRowIdentity $row $identity)) {
        throw "CommonLib relocation provider/symbol/kind binding is invalid."
    }
}

function Register-Fo4HdrRelocationIdentity {
    param([uint64]$Id, [uint64]$Rva, $Identity, $Seen)
    $providers = $Seen.Providers
    $symbols = $Seen.Symbols
    $ids = $Seen.Ids
    return -not ($id -eq 0 -or $rva -eq 0 -or
        -not $providers.Add($identity.Provider) -or
        -not $symbols.Add($identity.Symbol) -or
        -not $ids.Add($id))
}

function Register-Fo4HdrRelocationProfileRow {
    param($Row, $Identity, $Seen)
    Assert-Fo4HdrRelocationRowIdentity $Row $Identity
    try {
        $id = [UInt64]$row.id
        $rva = [UInt64]$row.rva
    }
    catch {
        throw "CommonLib relocation ID or RVA is invalid."
    }
    if (-not (Register-Fo4HdrRelocationIdentity $id $rva $identity $Seen)) {
        throw "CommonLib relocation profile is incomplete or duplicated."
    }
}

function Assert-Fo4HdrCommonLibRelocationProfile {
    param(
        [Parameter(Mandatory = $true)]
        [psobject]$Observation,

        [Parameter(Mandatory = $true)]
        [psobject]$CommonLibLock
    )

    Assert-Fo4HdrRelocationSourceIdentity $Observation $CommonLibLock

    $expected = @(Get-Fo4HdrRequiredRelocationProfile)
    $actual = @($Observation.relocations)
    if ($actual.Count -ne $expected.Count) {
        throw "CommonLib relocation profile must contain exactly eight rows."
    }

    $providers = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $symbols = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $ids = [Collections.Generic.HashSet[UInt64]]::new()
    $seen = @{Providers=$providers; Symbols=$symbols; Ids=$ids}
    for ($index = 0; $index -lt $expected.Count; ++$index) {
        $row = $actual[$index]
        $identity = $expected[$index]
        Register-Fo4HdrRelocationProfileRow $row $identity $seen
    }

    return $actual
}

function Test-Fo4HdrImageSpaceDimensions {
    param($Profile)
    return -not ([uint64]$Profile.managerSize -ne 0x1B8 -or
        [uint64]$Profile.currentEofDataOffset -ne 0x80 -or
        [uint64]$Profile.topLevelEffectCount -ne 72 -or
        $Profile.effects.Count -ne 72)
}

function Assert-Fo4HdrImageSpaceProfile {
    param(
        [Parameter(Mandatory = $true)]
        [psobject]$Profile
    )

    Assert-Fo4HdrImageSpaceSourceIdentity $Profile

    if (-not (Test-Fo4HdrImageSpaceDimensions $Profile)) {
        throw "Image-space semantic profile dimensions are invalid."
    }

    Assert-Fo4HdrImageSpaceLayout $Profile

    Assert-Fo4HdrImageSpaceEffects $Profile

    Assert-Fo4HdrImageSpaceActiveFields $Profile

    return $Profile
}

function Test-Fo4HdrImageSpaceCommonLibSource {
    param($CommonLibSource, $ExpectedCommonLibPaths)
    return -not ($commonLibSource.repository -ne
            "https://github.com/Dear-Modding-FO4/commonlibf4.git" -or
        $commonLibSource.commit -ne
            "2ac026b6b31a6ef517641be081b2a7b6b585a125" -or
        (Compare-Object `
            -ReferenceObject $expectedCommonLibPaths `
            -DifferenceObject @($commonLibSource.paths) `
            -SyncWindow 0))
}

function Assert-Fo4HdrImageSpaceSourceIdentity {
    param([psobject]$Profile)
    $commonLibSource = $Profile.sources.commonlib
    $expectedCommonLibPaths = @(
        "include/RE/I/ImageSpaceManager.h",
        "include/RE/I/ImageSpaceBaseData.h",
        "include/RE/I/ImageSpaceData.h",
        "include/RE/I/ImageSpaceEffect.h",
        "include/RE/I/ImageSpaceLUTData.h",
        "include/RE/N/NiTArray.h"
    )
    if (-not (Test-Fo4HdrImageSpaceCommonLibSource $commonLibSource $expectedCommonLibPaths)) {
        throw "Image-space semantic source identity is invalid."
    }

}

function Assert-Fo4HdrImageSpaceLayoutSection {
    param($Layout, [string]$SectionName, $ExpectedSection)
    $sectionProperty = $Layout.PSObject.Properties[$sectionName]
    if ($null -eq $sectionProperty) {
        throw "Image-space structural layout is incomplete."
    }
    $section = $sectionProperty.Value
    $actualNames = @($section.PSObject.Properties.Name)
    if ($actualNames.Count -ne $expectedSection.Count -or
        (Compare-Object `
            -ReferenceObject @($expectedSection.Keys) `
            -DifferenceObject $actualNames `
            -SyncWindow 0)) {
        throw "Image-space structural layout is incomplete."
    }
    foreach ($propertyName in $expectedSection.Keys) {
        if ([uint64]$section.$propertyName -ne
            [uint64]$expectedSection[$propertyName]) {
            throw "Image-space structural layout is invalid."
        }
    }
}

function Assert-Fo4HdrImageSpaceLayout {
    param([psobject]$Profile)
    $expectedLayout = [ordered]@{
        manager = [ordered]@{
            effectListOffset = 0x10
            baseDataOffset = 0x60
            overrideBaseDataOffset = 0x68
            underwaterBaseDataOffset = 0x70
            consoleBaseDataOffset = 0x78
            lutDataOffset = 0x130
            overrideLutDataOffset = 0x1A0
            readyOffset = 0x1B4
        }
        primitiveArray = [ordered]@{
            size = 0x18
            dataOffset = 0x08
            capacityOffset = 0x10
            freeIndexOffset = 0x12
            sizeOffset = 0x14
        }
        effect = [ordered]@{
            storedActiveOffset = 0x08
        }
        lut = [ordered]@{
            size = 0x70
            entryCount = 4
            weightOffset = 0x20
            textureOffset = 0x50
        }
    }
    $actualLayoutSections = @($Profile.layout.PSObject.Properties.Name)
    if ($actualLayoutSections.Count -ne $expectedLayout.Count -or
        (Compare-Object `
            -ReferenceObject @($expectedLayout.Keys) `
            -DifferenceObject $actualLayoutSections `
            -SyncWindow 0)) {
        throw "Image-space structural layout is incomplete."
    }
    foreach ($sectionName in $expectedLayout.Keys) {
        Assert-Fo4HdrImageSpaceLayoutSection $Profile.layout $sectionName $expectedLayout[$sectionName]
    }

}

function Register-Fo4HdrImageSpaceEffectIdentity {
    param($Effect, [int]$Index, [string]$Symbol, $Symbols)
    return -not ([uint64]$effect.index -ne [uint64]$index -or
            $symbol -notmatch '^EFFECT_[A-Z0-9_x]+$' -or
            -not $symbols.Add($symbol))
}

function Assert-Fo4HdrImageSpaceEffects {
    param([psobject]$Profile)
    $symbols = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal
    )
    for ($index = 0; $index -lt 72; ++$index) {
        $effect = $Profile.effects[$index]
        $symbol = [string]$effect.symbol
        if (-not (Register-Fo4HdrImageSpaceEffectIdentity $effect $index $symbol $symbols)) {
            throw "Image-space effect semantics are incomplete or unordered."
        }
    }
    $effectTable = (($Profile.effects | ForEach-Object {
        "{0}:{1}" -f $_.index, $_.symbol
    }) -join "`n") + "`n"
    $effectTableHash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData(
            [Text.UTF8Encoding]::new($false).GetBytes($effectTable)
        )
    ).ToLowerInvariant()
    if ($effectTableHash -ne
        "fd7918b8a53d003ea49d25ca82590af85571f2d5797dfd16ae343b45c4f94ba4") {
        throw "Image-space effect semantics differ from the reviewed table."
    }

}

function Test-Fo4HdrImageSpaceActiveField {
    param($Field, $Expected, $ActualAlias, $ActiveFields)
    $name = [string]$Field.field
    return -not ($activeFields.ContainsKey($name) -or
        [uint64]$field.offset -ne [uint64]$expected.offset -or
        $actualAlias -ne $expected.xeditAlias)
}

function Register-Fo4HdrImageSpaceActiveField {
    param($Field, $ExpectedFields, $ActiveFields)
    $name = [string]$field.field
    if (-not $expectedFields.Contains($name)) {
        throw "Image-space active-data semantics are invalid."
    }
    $expected = $expectedFields[$name]
    $actualAlias = if ($null -eq $field.PSObject.Properties["xeditAlias"]) {
        $null
    } else {
        [string]$field.xeditAlias
    }
    if (-not (Test-Fo4HdrImageSpaceActiveField $field $expected $actualAlias $activeFields)) {
        throw "Image-space active-data semantics are invalid."
    }
    $activeFields[$name] = [uint64]$field.offset
}

function Assert-Fo4HdrImageSpaceActiveFields {
    param([psobject]$Profile)
    $expectedFields = [ordered]@{
        "eyeAdaptSpeed" = @{ offset = 0x80; xeditAlias = $null }
        "bloomBlurRadius" = @{ offset = 0x84; xeditAlias = "Tonemap E" }
        "bloomThreshold" = @{ offset = 0x88; xeditAlias = $null }
        "bloomScale" = @{ offset = 0x8C; xeditAlias = $null }
        "receiveBloomThreshold" = @{
            offset = 0x90
            xeditAlias = "Auto Exposure Max"
        }
        "white" = @{ offset = 0x94; xeditAlias = "Auto Exposure Min" }
        "sunlightScale" = @{ offset = 0x98; xeditAlias = $null }
        "skyScale" = @{ offset = 0x9C; xeditAlias = $null }
        "eyeAdaptStrength" = @{ offset = 0xA0; xeditAlias = "Middle Gray" }
        "cinematicSaturation" = @{ offset = 0xA4; xeditAlias = $null }
        "cinematicBrightness" = @{ offset = 0xA8; xeditAlias = $null }
        "cinematicContrast" = @{ offset = 0xAC; xeditAlias = $null }
    }
    if ($Profile.activeData.Count -ne $expectedFields.Count) {
        throw "Image-space active-data profile has the wrong field count."
    }
    $activeFields = @{}
    foreach ($field in $Profile.activeData) {
        Register-Fo4HdrImageSpaceActiveField $field $expectedFields $activeFields
    }
    foreach ($name in $expectedFields.Keys) {
        if (-not $activeFields.ContainsKey($name)) {
            throw "Image-space active-data semantics are incomplete."
        }
    }

}
