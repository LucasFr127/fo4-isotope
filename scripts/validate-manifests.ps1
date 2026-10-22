# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

function Get-NativeRendererKeyStorage {
    param([long]$Getter, [byte[]]$Bytes, [int]$Offset)
    if ($offset -lt 0 -or $offset + 7 -gt $bytes.Count) {
        throw 'Native renderer key reference lies outside its fingerprint.'
    }
    $opcode = @($bytes[$offset], $bytes[$offset + 1], ($bytes[$offset + 2] -band 0xC7))
    if (($opcode -join ',') -ne '72,141,5') {
        throw 'Native renderer key getter requires a RIP-relative canonical-storage instruction.'
    }
    return $getter + $offset + 7 + [BitConverter]::ToInt32($bytes, $offset + 3)
}

function Assert-NativeRendererKeyRelationship {
    param([Parameter(Mandatory)]$Identity)
    $getter = [Convert]::ToInt64($Identity.keyGetterRva.Substring(2), 16)
    $storage = [Convert]::ToInt64($Identity.keyStorageRva.Substring(2), 16)
    if ($Identity.keyStorageAddressLibraryId -eq $Identity.keyGetterAddressLibraryId) {
        throw 'Native renderer key storage and getter require distinct mappings.'
    }
    $referencedStorage = Get-NativeRendererKeyStorage $getter `
        ([byte[]]@($Identity.keyGetterExpectedBytes)) ([int]$Identity.keyStorageReferenceOffset)
    if ($referencedStorage -ne $storage) {
        throw 'Native renderer key getter does not address its canonical pooled-key storage.'
    }
}

function Assert-WorkshopRendererKeyRelationship {
    param([Parameter(Mandatory)]$Identity)
    $initializer = [Convert]::ToInt64($Identity.keyInitializerRva.Substring(2), 16)
    $storage = [Convert]::ToInt64($Identity.keyStorageRva.Substring(2), 16)
    $bytes = [byte[]]@($Identity.keyInitializerExpectedBytes)
    if ($Identity.keyStorageAddressLibraryId -eq $Identity.keyInitializerAddressLibraryId) {
        throw 'Workshop renderer key storage and initializer require distinct mappings.'
    }
    if (($bytes[11..13] -join ',') -ne '72,141,13') {
        throw 'Workshop renderer key initializer requires a RIP-relative canonical-storage instruction.'
    }
    if ($initializer + 18 + [BitConverter]::ToInt32($bytes, 14) -ne $storage) {
        throw 'Workshop renderer key initializer does not address its canonical pooled-key storage.'
    }
}

function Assert-JsonSchemaFile {
    param($Check)
    if (-not (Test-Path $check.Json)) {
        throw "Missing JSON file: $($check.Json)"
    }
    if (-not (Test-Path $check.Schema)) {
        throw "Missing JSON schema: $($check.Schema)"
    }
    $jsonText = Get-Content $check.Json -Raw
    $valid = $jsonText | Test-Json -SchemaFile $check.Schema
    if (-not $valid) {
        throw "Schema validation failed: $($check.Json)"
    }
    Write-Host "Validated: $($check.Json)"
}

function Assert-MirrorValues {
    param([string]$Runtime, $Mirrors)
    foreach ($mirror in $mirrors.GetEnumerator()) {
        if ($mirror.Value[0] -cne $mirror.Value[1]) {
            throw (
                "Runtime $runtime manifest and lock disagree on " +
                "'$($mirror.Key)'."
            )
        }
    }
}

function Assert-RuntimeMirrors {
    param([string]$Runtime, $Manifest, $Entry, $Definition)
    $mirrors = [ordered]@{
        version = @([string]$manifest.runtime.version, "$runtime.0")
        status = @([string]$manifest.runtime.status, [string]$entry.status)
        platform = @([string]$manifest.runtime.platform, [string]$entry.platform)
        f4seVersion = @(
            [string]$manifest.runtime.f4seVersion,
            [string]$entry.f4seVersion
        )
        exeSha256 = @(
            [string]$manifest.runtime.exeSha256,
            [string]$entry.exeSha256
        )
        addressLibraryFile = @(
            [string]$manifest.runtime.addressLibraryFile,
            [string]$entry.addressLibraryFile
        )
    }
    Assert-MirrorValues $runtime $mirrors
    if ($entry.f4seVersion -cne $definition.F4SEVersion -or
        $entry.addressLibraryFile -cne $definition.AddressLibraryFile) {
        throw "Runtime $runtime lock differs from its project definition."
    }
}

function Assert-LockedRuntimeIdentity {
    param([string]$Runtime, [string]$ManifestPath, $Entry, $Definition)
    if ($entry.status -in @(
            "runtime-pinned-unvalidated",
            "capture-complete",
            "release-approved"
        )) {
        if ($entry.platform -notin @(
                $definition.AllowedPlatforms |
                    ForEach-Object { $_.ToLowerInvariant() }
            )) {
            throw "Runtime $runtime has an unapproved platform '$($entry.platform)'."
        }
        if ((Get-Fo4HdrCanonicalTextSha256 $manifestPath) -cne
            [string]$entry.runtimeManifestCanonicalSha256) {
            throw "Runtime $runtime manifest canonical identity differs from the lock."
        }
    }
}

function Assert-NativeLutRuntime {
    param([string]$Runtime, $Manifest)
    $lutProperty = $manifest.PSObject.Properties['nativeLutObserver']
    if ($null -ne $lutProperty -and $runtime -notin @('1.10.163', '1.10.984', '1.11.240')) {
        throw 'The native LUT observer has no qualified profile for this runtime.'
    }
}

function Test-ObserverCallRelationship {
    param($Call, [long]$Target, [hashtable]$Seen)
    $site = [Convert]::ToInt64($Call.rva.Substring(2), 16)
    $caller = [Convert]::ToInt64($Call.callerRva.Substring(2), 16)
    $bytes = [byte[]]@($Call.expectedBytes)
    if ($bytes.Count -ne 5 -or $bytes[0] -ne 232) { return $false }
    if ($site + 5 + [BitConverter]::ToInt32($bytes, 1) -ne $Target) { return $false }
    return $site -gt $caller -and -not $Seen.ContainsKey($site)
}

function Assert-ImageSpaceRuntime {
    param([string]$Runtime, $Manifest)
    $imageSpaceProperty = $manifest.PSObject.Properties['imageSpaceObserver']
    if ($null -eq $imageSpaceProperty) { return }
    if ($runtime -notin @('1.10.163', '1.10.984', '1.11.240')) {
        throw 'The ImageSpace provenance observer has no qualified profile for this runtime.'
    }
    $imageSpace = $imageSpaceProperty.Value
    $seenCalls = @{}
    foreach ($call in $imageSpace.calls) {
        $target = [Convert]::ToInt64($imageSpace.targets[$call.targetIndex].rva.Substring(2), 16)
        if (-not (Test-ObserverCallRelationship $call $target $seenCalls)) {
            throw 'ImageSpace provenance CALL identity/target/caller relationship is invalid.'
        }
        $seenCalls[[Convert]::ToInt64($call.rva.Substring(2), 16)] = $true
    }
}

function Assert-NativeUiCallSequence {
    param($Profile)
    $target = [Convert]::ToInt64($profile.target.rva.Substring(2), 16)
    $seenCalls = @{}
    for ($index = 0; $index -lt 2; ++$index) {
        $call = $profile.calls[$index]
        if ($call.slot -ne (24 + $index) -or
            -not (Test-ObserverCallRelationship $call $target $seenCalls)) {
            throw 'Native pause allocation CALL order/target/caller relationship is invalid.'
        }
        $seenCalls[[Convert]::ToInt64($call.rva.Substring(2), 16)] = $true
    }
}

function Assert-NativeUiResourcesRuntime {
    param([string]$Runtime, $Manifest)
    $profileProperty = $Manifest.PSObject.Properties['nativeUiResources']
    if ($null -eq $profileProperty) { return }
    if ($Runtime -notin @('1.10.163', '1.10.984', '1.11.240')) {
        throw 'Native pause resource allocation has no qualified profile for this runtime.'
    }
    Assert-NativeUiCallSequence $profileProperty.Value
}

function Assert-ScreenUiRuntime {
    param([string]$Runtime, $Manifest)
    $profileProperty = $Manifest.PSObject.Properties['experimentalScreenUiObserver']
    if ($null -eq $profileProperty) { return }
    if ($Runtime -notin @('1.10.163', '1.10.984', '1.11.240')) {
        throw 'The screen-space UI entry has no qualified profile for this runtime.'
    }
    $profile = $profileProperty.Value
    $target = [Convert]::ToInt64($profile.target.rva.Substring(2), 16)
    $seenCalls = @{}
    foreach ($call in $profile.calls) {
        if (-not (Test-ObserverCallRelationship $call $target $seenCalls)) {
            throw 'Screen-space UI CALL identity/target/caller relationship is invalid.'
        }
        $seenCalls[[Convert]::ToInt64($call.rva.Substring(2), 16)] = $true
    }
}

function Test-SceneCompositeCall {
    param($Call)
    # The schema fixes the five-byte shape; the site needs an offset and a rel32 CALL opcode.
    return [Convert]::ToInt64($Call.offset.Substring(2), 16) -gt 0 -and $Call.expectedBytes[0] -eq 232
}

function Assert-SceneCompositeRuntime {
    param([string]$Runtime, $Manifest)
    $profileProperty = $Manifest.PSObject.Properties['sceneCompositeObserver']
    if ($null -eq $profileProperty) { return }
    if ($Runtime -notin @('1.10.163', '1.10.984', '1.11.240')) {
        throw 'The deferred scene composite pass has no qualified profile for this runtime.'
    }
    if (-not (Test-SceneCompositeCall $profileProperty.Value.call)) {
        throw 'Deferred scene composite CALL offset or fingerprint is invalid.'
    }
}

function Assert-ArmorUiContract {
    param([string]$Runtime, $Armor)
    $validStatus = $armor.status -eq 'unvalidated' -and
        $armor.scope -eq 'power-armor-encoded-peak-composition'
    if ($runtime -notin @('1.10.163', '1.10.984', '1.11.240') -or -not $validStatus) {
        throw 'Armor observer runtime, status or scope is invalid.'
    }
    $expectedCalls = if ($runtime -eq '1.10.163') { 2 } else { 1 }
    if (@($armor.calls).Count -ne $expectedCalls) {
        throw 'Armor observer call count differs from the qualified runtime boundary.'
    }
}

function Assert-ArmorUiRuntime {
    param([string]$Runtime, $Manifest)
    $armorProperty = $manifest.PSObject.Properties['experimentalArmorUiObserver']
    if ($null -eq $armorProperty) { return }
    $armor = $armorProperty.Value
    Assert-ArmorUiContract $runtime $armor
    Assert-NativeRendererKeyRelationship $armor.hudRendererIdentity
    Assert-NativeRendererKeyRelationship $armor.flatRendererIdentity
    Assert-NativeRendererKeyRelationship $armor.armorRendererIdentity
    Assert-NativeRendererKeyRelationship $armor.rainRendererIdentity
    Assert-WorkshopRendererKeyRelationship $armor.workshopRendererIdentity
    $target = [Convert]::ToInt64($armor.target.rva.Substring(2), 16)
    $seenCalls = @{}
    foreach ($call in $armor.calls) {
        if (-not (Test-ObserverCallRelationship $call $target $seenCalls)) {
            throw 'Armor observer CALL identity/target/caller relationship is invalid.'
        }
        $seenCalls[[Convert]::ToInt64($call.rva.Substring(2), 16)] = $true
    }
}

$repoRoot = if ($RepoRoot) {
    [IO.Path]::GetFullPath($RepoRoot)
} else {
    Get-Fo4HdrRepoRoot
}
if (-not (Test-Path -LiteralPath $repoRoot -PathType Container)) {
    throw "Repository root does not exist: $repoRoot"
}
$checks = @(
    @{
        Json = Join-Path $repoRoot "versions.lock.json"
        Schema = Join-Path $repoRoot "schemas\versions-lock.schema.json"
    },
    @{
        Json = Join-Path $repoRoot "third_party\xmake-source-manifest.json"
        Schema = Join-Path $repoRoot "schemas\xmake-source-manifest.schema.json"
    }
)

foreach ($runtime in @(
    "1.10.163",
    "1.10.984",
    "1.11.240"
)) {
    $checks += @{
        Json = Join-Path $repoRoot "runtime\$runtime\manifest.json"
        Schema = Join-Path $repoRoot "schemas\runtime-manifest.schema.json"
    }
}

foreach ($check in $checks) {
    Assert-JsonSchemaFile $check
}

$lock = Get-Content -LiteralPath (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
foreach ($runtime in @(
    "1.10.163",
    "1.10.984",
    "1.11.240"
)) {
    $manifestPath = Join-Path $repoRoot "runtime\$runtime\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    $entry = $lock.runtimes.$runtime
    $definition = Get-Fo4HdrRuntimeDefinition $runtime
    Assert-RuntimeMirrors $runtime $manifest $entry $definition
    Assert-LockedRuntimeIdentity $runtime $manifestPath $entry $definition
    Assert-NativeLutRuntime $runtime $manifest
    Assert-ImageSpaceRuntime $runtime $manifest
    Assert-NativeUiResourcesRuntime $runtime $manifest
    Assert-ScreenUiRuntime $runtime $manifest
    Assert-SceneCompositeRuntime $runtime $manifest
    Assert-ArmorUiRuntime $runtime $manifest
}
