# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

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
    "1.11.221"
)) {
    $checks += @{
        Json = Join-Path $repoRoot "runtime\$runtime\manifest.json"
        Schema = Join-Path $repoRoot "schemas\runtime-manifest.schema.json"
    }
}

foreach ($check in $checks) {
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

$lock = Get-Content -LiteralPath (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
foreach ($runtime in @(
    "1.10.163",
    "1.10.984",
    "1.11.221"
)) {
    $manifestPath = Join-Path $repoRoot "runtime\$runtime\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    $entry = $lock.runtimes.$runtime
    $definition = Get-Fo4HdrRuntimeDefinition $runtime
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
    foreach ($mirror in $mirrors.GetEnumerator()) {
        if ($mirror.Value[0] -cne $mirror.Value[1]) {
            throw (
                "Runtime $runtime manifest and lock disagree on " +
                "'$($mirror.Key)'."
            )
        }
    }
    if ($entry.f4seVersion -cne $definition.F4SEVersion -or
        $entry.addressLibraryFile -cne $definition.AddressLibraryFile) {
        throw "Runtime $runtime lock differs from its project definition."
    }
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
    $lutProperty = $manifest.PSObject.Properties['nativeLutObserver']
    if ($null -ne $lutProperty -and $runtime -ne '1.10.163') {
        throw 'The native LUT observer is scoped to runtime 1.10.163.'
    }
    $imageSpaceProperty = $manifest.PSObject.Properties['imageSpaceObserver']
    if ($null -ne $imageSpaceProperty) {
        if ($runtime -ne '1.10.163') {
            throw 'The ImageSpace provenance observer is scoped to runtime 1.10.163.'
        }
        $imageSpace = $imageSpaceProperty.Value
        $seenCalls = @{}
        foreach ($call in $imageSpace.calls) {
            $site = [Convert]::ToInt64($call.rva.Substring(2), 16)
            $caller = [Convert]::ToInt64($call.callerRva.Substring(2), 16)
            $target = [Convert]::ToInt64($imageSpace.targets[$call.targetIndex].rva.Substring(2), 16)
            $bytes = [byte[]]@($call.expectedBytes)
            if ($bytes.Count -ne 5 -or $bytes[0] -ne 232 -or
                $site + 5 + [BitConverter]::ToInt32($bytes, 1) -ne $target -or
                $site -le $caller -or $seenCalls.ContainsKey($site)) {
                throw 'ImageSpace provenance CALL identity/target/caller relationship is invalid.'
            }
            $seenCalls[$site] = $true
        }
    }
    $armorProperty = $manifest.PSObject.Properties['experimentalArmorUiObserver']
    $armor = if ($null -ne $armorProperty) { $armorProperty.Value } else { $null }
    if ($null -ne $armor) {
        if ($runtime -ne '1.10.163' -or
            $armor.status -ne 'unvalidated' -or
            $armor.scope -ne
                'power-armor-encoded-peak-composition') {
            throw 'Armor observer runtime, status or scope is invalid.'
        }
        $target = [Convert]::ToInt64($armor.target.rva.Substring(2), 16)
        $seenCalls = @{}
        foreach ($call in $armor.calls) {
            $site = [Convert]::ToInt64($call.rva.Substring(2), 16)
            $caller = [Convert]::ToInt64($call.callerRva.Substring(2), 16)
            $bytes = [byte[]]@($call.expectedBytes)
            $displacement = [BitConverter]::ToInt32($bytes, 1)
            if ($bytes[0] -ne 232 -or $site + 5 + $displacement -ne $target -or
                $site -le $caller -or $seenCalls.ContainsKey($site)) {
                throw 'Armor observer CALL identity/target/caller relationship is invalid.'
            }
            $seenCalls[$site] = $true
        }
    }
}
