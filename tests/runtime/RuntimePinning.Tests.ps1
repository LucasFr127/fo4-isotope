# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

function Assert-Rejected {
    param([Parameter(Mandatory)][scriptblock]$Action, [string]$Case)
    try { & $Action } catch { return }
    throw "Runtime pinning policy accepted a forbidden case: $Case"
}

function Assert-ManifestProfileRejected {
    param([string]$Fixture, [string]$Profile, [string]$Case, [scriptblock]$Mutate)
    foreach ($runtime in @('1.10.163', '1.10.984', '1.11.240')) {
        $manifestPath = Join-Path $Fixture "runtime\$runtime\manifest.json"
        $sourcePath = Join-Path $repoRoot "runtime\$runtime\manifest.json"
        $identityManifest = Get-Content -LiteralPath $sourcePath -Raw | ConvertFrom-Json
        $selectedProfile = $identityManifest
        foreach ($part in $Profile.Split('.')) { $selectedProfile = $selectedProfile.$part }
        & $Mutate $selectedProfile
        Write-Fo4HdrCanonicalUtf8Text -Path $manifestPath `
            -Text (($identityManifest | ConvertTo-Json -Depth 32).Replace("`r`n", "`n") + "`n")
        $lockPath = Join-Path $Fixture 'versions.lock.json'
        $identityLock = Get-Content (Join-Path $repoRoot 'versions.lock.json') -Raw | ConvertFrom-Json
        $identityLock.runtimes.$runtime.runtimeManifestCanonicalSha256 =
            Get-Fo4HdrCanonicalTextSha256 $manifestPath
        Write-Fo4HdrCanonicalUtf8Text -Path $lockPath `
            -Text (($identityLock | ConvertTo-Json -Depth 32).Replace("`r`n", "`n") + "`n")
        Assert-Rejected { & $validator -RepoRoot $Fixture } "$runtime $Case"
        Copy-Item -LiteralPath $sourcePath -Destination $manifestPath -Force
        Copy-Item -LiteralPath (Join-Path $repoRoot 'versions.lock.json') -Destination $lockPath -Force
    }
}

Assert-Fo4HdrExactRuntimeFileVersion -Runtime "1.10.163" -FileVersion "1.10.163.0"
Assert-Rejected {
    Assert-Fo4HdrExactRuntimeFileVersion `
        -Runtime "1.10.163" `
        -FileVersion "1.10.1639.0"
} "prefix-near-match file version"

$locked = [pscustomobject]@{
    platform = "steam"
    exeSha256 = "a"
    f4seVersion = "0.6.23"
    f4seLoaderSha256 = "b"
    f4seRuntimeDllSha256 = "c"
    addressLibraryFile = "version-1-10-163-0.bin"
    addressLibrarySha256 = "d"
    shaderManifestSha256 = "FILL_AFTER_CAPTURE"
}
$observed = [pscustomobject]@{
    platform = "steam"
    fallout4Exe = [pscustomobject]@{ sha256 = "a" }
    f4seLoader = [pscustomobject]@{ sha256 = "b" }
    f4seRuntime = [pscustomobject]@{ version = "0.6.23"; sha256 = "c" }
    addressLibrary = [pscustomobject]@{
        file = "version-1-10-163-0.bin"; sha256 = "d"
    }
}
if (Test-Fo4HdrRuntimeIdentityChanged -LockedEntry $locked -Observation $observed) {
    throw "Identical runtime identity was reported as changed."
}
$observed.platform = "gog"
if (-not (Test-Fo4HdrRuntimeIdentityChanged -LockedEntry $locked -Observation $observed)) {
    throw "Runtime platform identity change was not detected."
}
$observed.platform = "steam"
$observed.addressLibrary.file = "version-1-10-984-0.bin"
if (-not (Test-Fo4HdrRuntimeIdentityChanged -LockedEntry $locked -Observation $observed)) {
    throw "Address Library semantic identity change was not detected."
}

$manifest = [pscustomobject]@{
    manualValidation = [pscustomobject]@{ complete = $false; report = $null }
    semanticStages = @()
    shaders = @()
    resourceRules = @()
}
if (Test-Fo4HdrIdentityBoundEvidencePresent -LockedEntry $locked -Manifest $manifest) {
    throw "Empty planned-runtime evidence was reported as retained evidence."
}
$manifest | Add-Member -NotePropertyName commonlibObservation -NotePropertyValue (
    [pscustomobject]@{ status = "runtime-validated" })
if (-not (Test-Fo4HdrIdentityBoundEvidencePresent -LockedEntry $locked -Manifest $manifest)) {
    throw "CommonLib runtime evidence was not recognized as identity-bound."
}
$manifest.commonlibObservation = $null
$evidenceDirectory = Join-Path `
    ([IO.Path]::GetTempPath()) `
    ("fo4hdr-runtime-pinning-" + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $evidenceDirectory | Out-Null
    Set-Content `
        -LiteralPath (Join-Path $evidenceDirectory 'accepted.json') `
        -Value '{}' `
        -Encoding utf8NoBOM
    if (-not (Test-Fo4HdrIdentityBoundEvidencePresent `
            -LockedEntry $locked `
            -Manifest $manifest `
            -EvidenceDirectory $evidenceDirectory)) {
        throw "Post-build runtime evidence was not recognized as identity-bound."
    }

    $fixture = Join-Path $evidenceDirectory "manifest-fixture"
    New-Item -ItemType Directory -Path $fixture | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot "schemas") `
        -Destination $fixture -Recurse
    Copy-Item -LiteralPath (Join-Path $repoRoot "versions.lock.json") `
        -Destination $fixture
    $thirdPartyRoot = Join-Path $fixture "third_party"
    New-Item -ItemType Directory -Path $thirdPartyRoot | Out-Null
    Copy-Item -LiteralPath (
        Join-Path $repoRoot "third_party\xmake-source-manifest.json"
    ) -Destination $thirdPartyRoot
    foreach ($runtime in @("1.10.163", "1.10.984", "1.11.240")) {
        $runtimeRoot = Join-Path $fixture "runtime\$runtime"
        New-Item -ItemType Directory -Path $runtimeRoot | Out-Null
        Copy-Item -LiteralPath (
            Join-Path $repoRoot "runtime\$runtime\manifest.json"
        ) -Destination $runtimeRoot
    }
    $validator = Join-Path $repoRoot "scripts\validate-manifests.ps1"
    & $validator -RepoRoot $fixture

    Assert-ManifestProfileRejected $fixture 'imageSpaceObserver' 'ImageSpace wrong CALL target' {
        param($profile) $profile.calls[0].expectedBytes[1] = $profile.calls[0].expectedBytes[1] -bxor 1
    }
    Assert-ManifestProfileRejected $fixture 'imageSpaceObserver' 'ImageSpace missing caller' {
        param($profile) $profile.calls[0].PSObject.Properties.Remove('callerAddressLibraryId')
    }

    foreach ($profile in @('hudRendererIdentity', 'flatRendererIdentity',
            'armorRendererIdentity', 'rainRendererIdentity')) {
        Assert-ManifestProfileRejected $fixture "experimentalArmorUiObserver.$profile" "$profile promoted status" {
            param($key) $key.status = 'runtime-validated'
        }
        Assert-ManifestProfileRejected $fixture "experimentalArmorUiObserver.$profile" "$profile missing storage mapping" {
            param($key) $key.PSObject.Properties.Remove('keyStorageAddressLibraryId')
        }
        Assert-ManifestProfileRejected $fixture "experimentalArmorUiObserver.$profile" "$profile executable storage" {
            param($key) $key.keyStorageSection = '.text'
        }
        Assert-ManifestProfileRejected $fixture "experimentalArmorUiObserver.$profile" "$profile incomplete fingerprint" {
            param($key) $key.keyGetterExpectedBytes = @($key.keyGetterExpectedBytes[0..24])
        }
        Assert-ManifestProfileRejected $fixture "experimentalArmorUiObserver.$profile" "$profile addresses another key" {
            param($key)
            $displacement = $key.keyStorageReferenceOffset + 3
            $key.keyGetterExpectedBytes[$displacement] = $key.keyGetterExpectedBytes[$displacement] -bxor 1
        }
        Assert-ManifestProfileRejected $fixture "experimentalArmorUiObserver.$profile" "$profile reuses one mapping" {
            param($key) $key.keyGetterAddressLibraryId = $key.keyStorageAddressLibraryId
        }
    }

    Assert-ManifestProfileRejected $fixture 'experimentalArmorUiObserver.workshopRendererIdentity' 'workshop promoted status' {
        param($key) $key.status = 'runtime-validated'
    }
    Assert-ManifestProfileRejected $fixture 'experimentalArmorUiObserver.workshopRendererIdentity' 'workshop missing storage mapping' {
        param($key) $key.PSObject.Properties.Remove('keyStorageAddressLibraryId')
    }
    Assert-ManifestProfileRejected $fixture 'experimentalArmorUiObserver.workshopRendererIdentity' 'workshop executable storage' {
        param($key) $key.keyStorageSection = '.text'
    }
    Assert-ManifestProfileRejected $fixture 'experimentalArmorUiObserver.workshopRendererIdentity' 'workshop incomplete fingerprint' {
        param($key) $key.keyInitializerExpectedBytes = @($key.keyInitializerExpectedBytes[0..37])
    }
    Assert-ManifestProfileRejected $fixture 'experimentalArmorUiObserver.workshopRendererIdentity' 'workshop addresses another key' {
        param($key) $key.keyInitializerExpectedBytes[14] = $key.keyInitializerExpectedBytes[14] -bxor 1
    }
    Assert-ManifestProfileRejected $fixture 'experimentalArmorUiObserver.workshopRendererIdentity' 'workshop reuses one mapping' {
        param($key) $key.keyInitializerAddressLibraryId = $key.keyStorageAddressLibraryId
    }

    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation mode changed' {
        param($profile) $profile.mode = 0
    }
    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation format changed' {
        param($profile) $profile.promotedFormat = 24
    }
    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation ABI changed' {
        param($profile) $profile.abi = 'void()'
    }
    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation missing mapping' {
        param($profile) $profile.target.PSObject.Properties.Remove('addressLibraryId')
    }
    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation wrong CALL target' {
        param($profile) $profile.calls[0].expectedBytes[1] = $profile.calls[0].expectedBytes[1] -bxor 1
    }
    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation duplicate CALL' {
        param($profile) $profile.calls[1].rva = $profile.calls[0].rva
    }
    Assert-ManifestProfileRejected $fixture 'nativeUiResources' 'pause allocation reversed slots' {
        param($profile) $profile.calls[0].slot = 25
    }

    Assert-ManifestProfileRejected $fixture 'experimentalScreenUiObserver' 'screen UI ABI changed' {
        param($profile) $profile.abi = 'void(UI*,uint32_t)'
    }
    Assert-ManifestProfileRejected $fixture 'experimentalScreenUiObserver' 'screen UI missing mapping' {
        param($profile) $profile.calls[3].PSObject.Properties.Remove('callerAddressLibraryId')
    }
    Assert-ManifestProfileRejected $fixture 'experimentalScreenUiObserver' 'screen UI wrong CALL target' {
        param($profile) $profile.calls[0].expectedBytes[1] = $profile.calls[0].expectedBytes[1] -bxor 1
    }
    Assert-ManifestProfileRejected $fixture 'experimentalScreenUiObserver' 'screen UI duplicate CALL' {
        param($profile) $profile.calls[3].rva = $profile.calls[0].rva
    }
    Assert-ManifestProfileRejected $fixture 'experimentalScreenUiObserver' 'screen UI incomplete fingerprint' {
        param($profile) $profile.target.expectedBytes = @($profile.target.expectedBytes[0..30])
    }

    $fixtureLock = Join-Path $fixture "versions.lock.json"
    $invalidLock = Get-Content -LiteralPath $fixtureLock -Raw |
        ConvertFrom-Json
    $invalidLock.runtimes.'1.10.163'.f4seLoaderSha256 = "UNVALIDATED"
    [IO.File]::WriteAllText(
        $fixtureLock,
        ($invalidLock | ConvertTo-Json -Depth 32),
        [Text.UTF8Encoding]::new($false)
    )
    Assert-Rejected {
        & $validator -RepoRoot $fixture
    } "active runtime with a placeholder identity"
    Copy-Item -LiteralPath (Join-Path $repoRoot "versions.lock.json") `
        -Destination $fixtureLock -Force

    $plannedManifestPath = Join-Path $fixture "runtime\1.10.984\manifest.json"
    $plannedManifest = Get-Content -LiteralPath $plannedManifestPath -Raw |
        ConvertFrom-Json
    $plannedManifest.runtime.f4seVersion = "0.7.3"
    Write-Fo4HdrJsonFile `
        -Value $plannedManifest `
        -Path $plannedManifestPath `
        -SchemaPath (Join-Path $fixture "schemas\runtime-manifest.schema.json")
    Assert-Rejected {
        & $validator -RepoRoot $fixture
    } "runtime manifest that differs from the lock"
}
finally {
    if (Test-Path -LiteralPath $evidenceDirectory) {
        Remove-Item -LiteralPath $evidenceDirectory -Recurse -Force
    }
}

Write-Host "Runtime pinning policy regression tests passed."
