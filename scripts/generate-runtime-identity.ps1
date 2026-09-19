# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet("1.10.163")]
    [string]$Runtime = "1.10.163",

    [string]$OutputPath,

    [switch]$Check
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$lock = Get-Content (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
$entry = $lock.runtimes.$Runtime
$definition = Get-Fo4HdrRuntimeDefinition $Runtime
$manifestPath =
    Join-Path $repoRoot "runtime\$Runtime\manifest.json"
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json

$lateGameUiShaders = @($manifest.shaders | Where-Object {
    [string]$_.name -eq "late-game-ui-composite"
})
if ($lateGameUiShaders.Count -ne 1) {
    throw "Runtime manifest must contain one late-game-ui-composite shader."
}
$lateGameUiShader = $lateGameUiShaders[0]
$lateGameUiHash = [string]$lateGameUiShader.dxbcHash
if ($lateGameUiShader.stage -ne "pixel" -or
    $lateGameUiShader.signal -ne "straight-srgb-rgba" -or
    $lateGameUiShader.sourceTexture.format -ne "R8G8B8A8_UNORM" -or
    $lateGameUiShader.sourceTexture.extent -ne "presentation" -or
    $lateGameUiShader.role -ne "final-native-ui-composite" -or
    $lateGameUiHash -notmatch '^[0-9a-f]{8}(-[0-9a-f]{8}){3}$') {
    throw "The late-game UI shader fact is incomplete or malformed."
}
$lateGameUiHashWords = @($lateGameUiHash.Split("-") | ForEach-Object {
    "0x$($_)u"
})

$lateGameUiBackdropShaders = @($manifest.shaders | Where-Object {
    [string]$_.name -eq "late-game-ui-gamma-backdrop"
})
if ($lateGameUiBackdropShaders.Count -ne 1) {
    throw "Runtime manifest must contain one late-game-ui-gamma-backdrop shader."
}
$lateGameUiBackdropShader = $lateGameUiBackdropShaders[0]
$lateGameUiBackdropHash = [string]$lateGameUiBackdropShader.dxbcHash
if ($lateGameUiBackdropShader.stage -ne "pixel" -or
    $lateGameUiBackdropShader.signal -ne
        "sdr-display-code-rgb-with-coverage-alpha" -or
    $lateGameUiBackdropShader.draw.kind -ne "indexed" -or
    $lateGameUiBackdropShader.draw.indexCount -ne 6 -or
    $lateGameUiBackdropShader.draw.depth -ne "none" -or
    $lateGameUiBackdropShader.role -ne
        "late-native-ui-gamma-backdrop" -or
    $lateGameUiBackdropHash -notmatch '^[0-9a-f]{8}(-[0-9a-f]{8}){3}$') {
    throw "The late-game UI backdrop shader fact is incomplete or malformed."
}
$lateGameUiBackdropHashWords = @(
    $lateGameUiBackdropHash.Split("-") | ForEach-Object {
        "0x$($_)u"
    }
)

if (-not $OutputPath) {
    $OutputPath = Join-Path `
        $repoRoot `
        "runtime\$Runtime\RuntimeIdentity.generated.hpp"
}

$manifestVersion = [string]$manifest.runtime.version
if ($manifestVersion -ne "$Runtime.0" -or
    $manifest.runtime.f4seVersion -ne $entry.f4seVersion -or
    $manifest.runtime.status -ne $entry.status -or
    $manifest.runtime.platform -ne $entry.platform -or
    $manifest.runtime.exeSha256 -ne $entry.exeSha256 -or
    $manifest.runtime.addressLibraryFile -ne
        $entry.addressLibraryFile) {
    throw (
        "Runtime manifest and versions.lock.json disagree for $Runtime."
    )
}
if ($entry.f4seVersion -ne $definition.F4SEVersion -or
    $entry.addressLibraryFile -ne $definition.AddressLibraryFile -or
    $entry.platform -notin (
        $definition.AllowedPlatforms |
        ForEach-Object { $_.ToLowerInvariant() }
    ) -or
    $entry.status -notin @(
        "runtime-pinned-unvalidated",
        "capture-complete",
        "release-approved"
    )) {
    throw "Runtime lock differs from the project runtime definition."
}

$manifestHash =
    Get-Fo4HdrCanonicalTextSha256 $manifestPath
if ($entry.runtimeManifestCanonicalSha256 -notmatch
        '^[0-9a-f]{64}$' -or
    $manifestHash -ne
        $entry.runtimeManifestCanonicalSha256) {
    throw (
        "The canonical runtime manifest hash is not locked for $Runtime."
    )
}

foreach ($property in @(
    "exeSha256",
    "f4seLoaderSha256",
    "f4seRuntimeDllSha256",
    "addressLibrarySha256"
)) {
    if ([string]$entry.$property -notmatch '^[0-9a-f]{64}$') {
        throw "Runtime identity '$property' is not initialized for $Runtime."
    }
}
if ($lock.commonlib.commit -notmatch '^[0-9a-f]{40}$') {
    throw "CommonLibF4 commit is not initialized."
}

if ($manifest.commonlibObservation.status -ne
        "implementation-pending-runtime-validation" -and
    $manifest.commonlibObservation.status -ne "runtime-validated") {
    throw "CommonLib observation status is missing or invalid for $Runtime."
}
$null = Assert-Fo4HdrCommonLibRelocationProfile `
    -Observation $manifest.commonlibObservation `
    -CommonLibLock $lock.commonlib
$imageSpaceProfile = Assert-Fo4HdrImageSpaceProfile `
    $manifest.commonlibObservation.imageSpaceProfile
$imageSpaceLayout = $imageSpaceProfile.layout
if ([string]$imageSpaceProfile.sources.commonlib.repository -ne
        [string]$lock.commonlib.repository -or
    [string]$imageSpaceProfile.sources.commonlib.commit -ne
        [string]$lock.commonlib.commit) {
    throw (
        "The image-space profile CommonLib source differs from the " +
        "active dependency lock."
    )
}
$effectTableText = (($imageSpaceProfile.effects | ForEach-Object {
    "{0}:{1}" -f $_.index, $_.symbol
}) -join "`n") + "`n"
$effectTableHash = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData(
        [Text.UTF8Encoding]::new($false).GetBytes($effectTableText)
    )
).ToLowerInvariant()
$effectSemanticLines = @(
    for ($index = 0; $index -lt $imageSpaceProfile.effects.Count; ++$index) {
        $symbol = [string]$imageSpaceProfile.effects[$index].symbol
        $suffix = if ($index -eq $imageSpaceProfile.effects.Count - 1) {
            ""
        } else {
            ","
        }
        "    ImageSpaceEffectSemantic{$index, `"$symbol`"}$suffix"
    }
)
$effectEnumComparisonLines = @(
    for ($index = 0; $index -lt $imageSpaceProfile.effects.Count; ++$index) {
        $semantic = $imageSpaceProfile.effects[$index]
        $suffix = if ($index -eq $imageSpaceProfile.effects.Count - 1) {
            ""
        } else {
            ","
        }
        "        static_cast<std::uint16_t>(EffectEnum::$($semantic.symbol)) == $($semantic.index)$suffix"
    }
)
$activeOffsetsByName = @{}
$activeAliasesByName = @{}
foreach ($field in $imageSpaceProfile.activeData) {
    $activeOffsetsByName[[string]$field.field] = [uint64]$field.offset
    $activeAliasesByName[[string]$field.field] =
        if ($null -eq $field.PSObject.Properties["xeditAlias"]) {
            ""
        } else {
            [string]$field.xeditAlias
        }
}
$activeFieldOrder = @(
    "eyeAdaptSpeed",
    "bloomBlurRadius",
    "bloomThreshold",
    "bloomScale",
    "receiveBloomThreshold",
    "white",
    "sunlightScale",
    "skyScale",
    "eyeAdaptStrength",
    "cinematicSaturation",
    "cinematicBrightness",
    "cinematicContrast"
)
$activeOffsetLines = @(
    for ($index = 0; $index -lt $activeFieldOrder.Count; ++$index) {
        $suffix = if ($index -eq $activeFieldOrder.Count - 1) { "" } else { "," }
        "    $($activeOffsetsByName[$activeFieldOrder[$index]])$suffix"
    }
)
$activeSemanticLines = @(
    for ($index = 0; $index -lt $activeFieldOrder.Count; ++$index) {
        $field = $activeFieldOrder[$index]
        $suffix = if ($index -eq $activeFieldOrder.Count - 1) { "" } else { "," }
        "    ImageSpaceActiveDataSemantic{`"$field`", $($activeOffsetsByName[$field]), `"$($activeAliasesByName[$field])`"}$suffix"
    }
)

$armor = $manifest.experimentalArmorUiObserver
if ($armor.status -ne 'unvalidated' -or
    $armor.scope -ne 'power-armor-encoded-peak-composition' -or
    $armor.abi -ne 'void(Renderer*,uint32_t)' -or @($armor.calls).Count -ne 2 -or
    @($armor.target.expectedBytes).Count -ne 23) {
    throw 'The armor submission observer contract is incomplete.'
}
$armorLines = @(
    "inline constexpr std::uintptr_t kArmorUiTargetRva = $($armor.target.rva);",
    "inline constexpr std::uint64_t kArmorUiTargetId = $($armor.target.addressLibraryId);",
    "inline constexpr std::array<std::uint8_t, 23> kArmorUiTargetBytes{{ $($armor.target.expectedBytes -join ', ') }};",
    "inline constexpr std::array<std::uintptr_t, 2> kArmorUiCallerRvas{{ $($armor.calls.callerRva -join ', ') }};",
    "inline constexpr std::array<std::uint64_t, 2> kArmorUiCallerIds{{ $($armor.calls.callerAddressLibraryId -join ', ') }};",
    "inline constexpr std::array<std::uintptr_t, 2> kArmorUiCallRvas{{ $($armor.calls.rva -join ', ') }};",
    'inline constexpr std::array<std::array<std::uint8_t, 5>, 2> kArmorUiCallBytes{{'
)
foreach ($call in $armor.calls) {
    if (@($call.expectedBytes).Count -ne 5 -or $call.expectedBytes[0] -ne 232) {
        throw 'The armor observer requires two exact direct CALL fingerprints.'
    }
    $armorLines += "    {{ $($call.expectedBytes -join ', ') }},"
}
$armorLines += '}};'

$lut = $manifest.nativeLutObserver
if ($lut.status -ne 'unvalidated' -or
    $lut.scope -ne 'synchronous-gamma-correct-lut-render' -or
    $lut.abi -ne 'void(ImageSpaceEffect*,BSTriShape*,ImageSpaceEffectParam*)' -or
    $lut.effectSubobjectOffset -ne 280 -or $lut.textureSrvOffset -ne 0 -or
    @($lut.target.expectedBytes).Count -ne 25) {
    throw 'The native LUT observation contract is incomplete.'
}
$lutLines = @(
    "inline constexpr std::uintptr_t kNativeLutRenderRva = $($lut.target.rva);",
    "inline constexpr std::uint64_t kNativeLutRenderId = $($lut.target.addressLibraryId);",
    "inline constexpr std::size_t kNativeLutEffectSubobjectOffset = $($lut.effectSubobjectOffset);",
    "inline constexpr std::size_t kNativeLutTextureSrvOffset = $($lut.textureSrvOffset);",
    "inline constexpr std::array<std::uint8_t, 25> kNativeLutRenderBytes{{ $($lut.target.expectedBytes -join ', ') }};"
)

$imageSpace = $manifest.imageSpaceObserver
$targetNames = @('Weather', 'Accumulate', 'RoomBlend', 'SetLut')
$targetAbis = @('void(Sky*)', 'void(ImageSpaceBaseData*,const ImageSpaceBaseData*,float)',
    'void(ImageSpaceBaseData*,const ImageSpaceBaseData*,const ImageSpaceBaseData*,float)',
    'void(ImageSpaceManager*,const ImageSpaceLUTData*)')
if ($imageSpace.status -ne 'unvalidated' -or
    $imageSpace.scope -ne 'weather-imagespace-provenance' -or
    @($imageSpace.targets).Count -ne 4 -or @($imageSpace.calls).Count -ne 11) {
    throw 'The ImageSpace provenance contract is incomplete.'
}
$imageSpaceLines = @()
for ($index = 0; $index -lt 4; ++$index) {
    $target = $imageSpace.targets[$index]
    if ($target.name -cne $targetNames[$index] -or $target.abi -cne $targetAbis[$index]) {
        throw 'The ImageSpace provenance target order/ABI is invalid.'
    }
    $prefix = 'kImageSpace' + $target.name
    $imageSpaceLines += @(
        "inline constexpr std::uintptr_t $($prefix)Rva = $($target.rva);",
        "inline constexpr std::uint64_t $($prefix)Id = $($target.addressLibraryId);",
        "inline constexpr std::array<std::uint8_t, $(@($target.expectedBytes).Count)> $($prefix)Bytes{{ $($target.expectedBytes -join ', ') }};"
    )
}
foreach ($name in @('Lut', 'Base')) {
    $global = $imageSpace.globals.($name.ToLowerInvariant())
    $imageSpaceLines += @(
        "inline constexpr std::uintptr_t kImageSpaceGlobal$($name)Rva = $($global.rva);",
        "inline constexpr std::uint64_t kImageSpaceGlobal$($name)Id = $($global.addressLibraryId);"
    )
}
$imageSpaceLines += @(
    "inline constexpr std::array<std::uintptr_t, 11> kImageSpaceCallRvas{{ $($imageSpace.calls.rva -join ', ') }};",
    "inline constexpr std::array<std::uintptr_t, 11> kImageSpaceCallerRvas{{ $($imageSpace.calls.callerRva -join ', ') }};",
    "inline constexpr std::array<std::uint64_t, 11> kImageSpaceCallerIds{{ $($imageSpace.calls.callerAddressLibraryId -join ', ') }};",
    "inline constexpr std::array<std::uint32_t, 11> kImageSpaceCallTargetIndices{{ $($imageSpace.calls.targetIndex -join ', ') }};",
    'inline constexpr std::array<std::array<std::uint8_t, 5>, 11> kImageSpaceCallBytes{{'
)
$expectedTargetIndices = @(0, 0, 0, 1, 1, 1, 1, 2, 3, 3, 3)
for ($index = 0; $index -lt 11; ++$index) {
    $call = $imageSpace.calls[$index]
    if ($call.targetIndex -ne $expectedTargetIndices[$index] -or
        @($call.expectedBytes).Count -ne 5 -or $call.expectedBytes[0] -ne 232) {
        throw 'The ImageSpace provenance call order/fingerprint is invalid.'
    }
    $imageSpaceLines += "    {{ $($call.expectedBytes -join ', ') }},"
}
$imageSpaceLines += '}};'

$lines = @(
    "// SPDX-License-Identifier: GPL-3.0-or-later",
    "// Generated by scripts/generate-runtime-identity.ps1. Do not edit.",
    "#pragma once",
    "",
    "#include <array>",
    "#include <cstddef>",
    "#include <cstdint>",
    "#include <string_view>",
    "",
    "namespace fo4hdr::generated {",
    ($armorLines -join "`n"),
    ($lutLines -join "`n"),
    ($imageSpaceLines -join "`n"),
    "",
    "inline constexpr std::string_view kRuntimeVersion = `"$manifestVersion`";",
    "inline constexpr std::string_view kRuntimeStatus = `"$($entry.status)`";",
    "inline constexpr std::string_view kExecutableSha256 = `"$($entry.exeSha256)`";",
    "inline constexpr std::string_view kF4seVersion = `"$($entry.f4seVersion)`";",
    "inline constexpr std::string_view kF4seLoaderSha256 = `"$($entry.f4seLoaderSha256)`";",
    "inline constexpr std::string_view kF4seRuntimeFile = `"$($definition.RuntimeDll)`";",
    "inline constexpr std::string_view kF4seRuntimeSha256 = `"$($entry.f4seRuntimeDllSha256)`";",
    "inline constexpr std::string_view kAddressLibraryFile = `"$($entry.addressLibraryFile)`";",
    "inline constexpr std::string_view kAddressLibrarySha256 = `"$($entry.addressLibrarySha256)`";",
    "inline constexpr std::string_view kManifestSha256 = `"$manifestHash`";",
    "inline constexpr std::string_view kCommonLibCommit = `"$($lock.commonlib.commit)`";",
    "inline constexpr std::array<std::uint32_t, 4> kLateGameUiCompositeDxbcHash{{",
    "    $($lateGameUiHashWords -join ', ')",
    "}};",
    "inline constexpr std::array<std::uint32_t, 4> kLateGameUiBackdropDxbcHash{{",
    "    $($lateGameUiBackdropHashWords -join ', ')",
    "}};",
    "struct ImageSpaceEffectSemantic",
    "{",
    "    std::uint16_t index;",
    "    std::string_view symbol;",
    "};",
    "",
    "struct ImageSpaceActiveDataSemantic",
    "{",
    "    std::string_view field;",
    "    std::size_t offset;",
    "    std::string_view xeditAlias;",
    "};",
    "",
    "template <class EffectEnum>",
    "inline constexpr std::array kImageSpaceEffectIndexMatches{"
) + $effectEnumComparisonLines + @(
    "};",
    "",
    "template <class EffectEnum>",
    "consteval bool ImageSpaceEffectEnumMatches()",
    "{",
    "    for (const bool match : kImageSpaceEffectIndexMatches<EffectEnum>)",
    "    {",
    "        if (!match) return false;",
    "    }",
    "    return true;",
    "}",
    "",
    "inline constexpr std::size_t kImageSpaceManagerSize = $($imageSpaceProfile.managerSize);",
    "inline constexpr std::size_t kImageSpaceCurrentEofDataOffset = $($imageSpaceProfile.currentEofDataOffset);",
    "inline constexpr std::size_t kImageSpaceEffectListOffset = $($imageSpaceLayout.manager.effectListOffset);",
    "inline constexpr std::size_t kImageSpaceBaseDataOffset = $($imageSpaceLayout.manager.baseDataOffset);",
    "inline constexpr std::size_t kImageSpaceOverrideBaseDataOffset = $($imageSpaceLayout.manager.overrideBaseDataOffset);",
    "inline constexpr std::size_t kImageSpaceUnderwaterBaseDataOffset = $($imageSpaceLayout.manager.underwaterBaseDataOffset);",
    "inline constexpr std::size_t kImageSpaceConsoleBaseDataOffset = $($imageSpaceLayout.manager.consoleBaseDataOffset);",
    "inline constexpr std::size_t kImageSpaceLutDataOffset = $($imageSpaceLayout.manager.lutDataOffset);",
    "inline constexpr std::size_t kImageSpaceOverrideLutDataOffset = $($imageSpaceLayout.manager.overrideLutDataOffset);",
    "inline constexpr std::size_t kImageSpaceReadyOffset = $($imageSpaceLayout.manager.readyOffset);",
    "inline constexpr std::size_t kImageSpacePrimitiveArraySize = $($imageSpaceLayout.primitiveArray.size);",
    "inline constexpr std::size_t kImageSpacePrimitiveArrayDataOffset = $($imageSpaceLayout.primitiveArray.dataOffset);",
    "inline constexpr std::size_t kImageSpacePrimitiveArrayCapacityOffset = $($imageSpaceLayout.primitiveArray.capacityOffset);",
    "inline constexpr std::size_t kImageSpacePrimitiveArrayFreeIndexOffset = $($imageSpaceLayout.primitiveArray.freeIndexOffset);",
    "inline constexpr std::size_t kImageSpacePrimitiveArraySizeOffset = $($imageSpaceLayout.primitiveArray.sizeOffset);",
    "inline constexpr std::size_t kImageSpaceEffectStoredActiveOffset = $($imageSpaceLayout.effect.storedActiveOffset);",
    "inline constexpr std::size_t kImageSpaceLutDataSize = $($imageSpaceLayout.lut.size);",
    "inline constexpr std::size_t kImageSpaceLutEntryCount = $($imageSpaceLayout.lut.entryCount);",
    "inline constexpr std::size_t kImageSpaceLutWeightOffset = $($imageSpaceLayout.lut.weightOffset);",
    "inline constexpr std::size_t kImageSpaceLutTextureOffset = $($imageSpaceLayout.lut.textureOffset);",
    "inline constexpr std::uint16_t kImageSpaceTopLevelEffectCount = $($imageSpaceProfile.topLevelEffectCount);",
    "inline constexpr std::string_view kImageSpaceCommonLibSourceCommit = `"$($imageSpaceProfile.sources.commonlib.commit)`";",
    "inline constexpr std::string_view kImageSpaceXEditAliasSourceCommit = `"$($imageSpaceProfile.sources.xeditFieldAliases.commit)`";",
    "inline constexpr std::string_view kImageSpaceEffectTableSha256 = `"$effectTableHash`";",
    "inline constexpr std::array<ImageSpaceEffectSemantic, kImageSpaceTopLevelEffectCount>",
    "    kImageSpaceEffectSemantics{{"
) + $effectSemanticLines + @(
    "    }};",
    "inline constexpr std::array<std::size_t, 12> kImageSpaceActiveDataOffsets{{"
) + $activeOffsetLines + @(
    "    }};",
    "inline constexpr std::array<ImageSpaceActiveDataSemantic, 12>",
    "    kImageSpaceActiveDataSemantics{{"
) + $activeSemanticLines + @(
    "    }};",
    "",
    "}  // namespace fo4hdr::generated",
    ""
)
$generated = $lines -join "`n"

if ($Check) {
    if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        throw "Generated runtime identity is missing: $OutputPath"
    }
    $current = Read-Fo4HdrCanonicalUtf8Text $OutputPath
    if ($current -ne $generated) {
        throw (
            "Generated runtime identity is stale. Run " +
            "scripts/generate-runtime-identity.ps1 -Runtime $Runtime."
        )
    }
    Write-Host "Generated runtime identity is current for $Runtime."
    return
}

Write-Fo4HdrCanonicalUtf8Text `
    -Path $OutputPath `
    -Text $generated
Write-Host "Generated runtime identity: $OutputPath"
