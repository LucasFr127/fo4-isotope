# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "Project.Runtime.Binary.ps1")
. (Join-Path $PSScriptRoot "Project.Runtime.Profiles.ps1")

function ConvertTo-Fo4HdrPackedPluginVersion {
    param([string]$Version)
    $parts = $Version.Split('.') | ForEach-Object { [uint32]$_ }
    return (($parts[0] -band 0xFF) -shl 24) -bor
        (($parts[1] -band 0xFF) -shl 16) -bor
        (($parts[2] -band 0xFFF) -shl 4)
}

function Test-Fo4HdrRuntimeActionAuthorization {
    param([string]$Status, [string]$Action, [bool]$AllowUnvalidatedRuntime)
    $allowed = switch ($Action) {
        "developer-build" {
            $Status -in @(
                "runtime-pinned-unvalidated",
                "capture-complete",
                "release-approved"
            ) -or ($AllowUnvalidatedRuntime -and $Status -eq "planned")
            break
        }
        "runtime-identity-validation" {
            $Status -in @(
                "runtime-pinned-unvalidated",
                "capture-complete",
                "release-approved"
            )
            break
        }
        "release-packaging" {
            $Status -eq "release-approved"
            break
        }
    }

    return $allowed
}

function Assert-Fo4HdrRuntimeActionAllowed {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Status,

        [Parameter(Mandatory = $true)]
        [ValidateSet(
            "developer-build",
            "runtime-identity-validation",
            "release-packaging"
        )]
        [string]$Action,

        [switch]$AllowUnvalidatedRuntime
    )

    $knownStatuses = @(
        "primary-uninitialized",
        "planned",
        "runtime-pinned-unvalidated",
        "capture-complete",
        "release-approved"
    )
    if ($Status -notin $knownStatuses) {
        throw "Unknown FO4Isotope runtime status '$Status'."
    }

    $allowed = Test-Fo4HdrRuntimeActionAuthorization $Status $Action $AllowUnvalidatedRuntime.IsPresent

    if (-not $allowed) {
        $overrideHint = if ($Action -eq "developer-build" -and
            $Status -eq "planned") {
            " Pass -AllowUnvalidatedRuntime only for explicitly scoped " +
            "runtime-porting work."
        } else {
            ""
        }
        throw (
            "Runtime status '$Status' does not authorize action '$Action'." +
            $overrideHint
        )
    }
}

function Assert-Fo4HdrExactRuntimeFileVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Runtime,
        [Parameter(Mandatory = $true)][string]$FileVersion
    )

    if ($FileVersion -cne "$Runtime.0") {
        throw "Expected exact runtime $Runtime.0; found $FileVersion"
    }
}

function Test-Fo4HdrRuntimeIdentityChanged {
    param(
        [Parameter(Mandatory = $true)]$LockedEntry,
        [Parameter(Mandatory = $true)]$Observation
    )

    $pairs = @(
        @($LockedEntry.platform, $Observation.platform),
        @($LockedEntry.exeSha256, $Observation.fallout4Exe.sha256),
        @($LockedEntry.f4seVersion, $Observation.f4seRuntime.version),
        @($LockedEntry.f4seLoaderSha256, $Observation.f4seLoader.sha256),
        @($LockedEntry.f4seRuntimeDllSha256, $Observation.f4seRuntime.sha256),
        @($LockedEntry.addressLibraryFile, $Observation.addressLibrary.file),
        @($LockedEntry.addressLibrarySha256, $Observation.addressLibrary.sha256)
    )
    foreach ($pair in $pairs) {
        if ([string]$pair[0] -in @("", "unvalidated", "UNVALIDATED")) {
            continue
        }
        if (-not [string]::Equals(
            [string]$pair[0],
            [string]$pair[1],
            [StringComparison]::OrdinalIgnoreCase
        )) {
            return $true
        }
    }
    return $false
}

function Test-Fo4HdrEvidenceDirectory {
    param([string]$EvidenceDirectory)
    return $EvidenceDirectory -and
        (Test-Path -LiteralPath $EvidenceDirectory -PathType Container) -and
        @(Get-ChildItem `
            -LiteralPath $EvidenceDirectory `
            -Filter '*.json' `
            -File).Count -ne 0
}

function Test-Fo4HdrCommonLibObservationEvidence {
    param($Manifest)
    $commonlibProperty = $Manifest.PSObject.Properties["commonlibObservation"]
    return $null -ne $commonlibProperty -and
        $null -ne $commonlibProperty.Value -and
        [string]$commonlibProperty.Value.status -ne ""
}

function Test-Fo4HdrSemanticEvidence {
    param($Manifest)
    return (@($Manifest.semanticStages).Count -ne 0 -or
        @($Manifest.shaders).Count -ne 0 -or
        @($Manifest.resourceRules).Count -ne 0)
}

function Test-Fo4HdrIdentityBoundEvidencePresent {
    param(
        [Parameter(Mandatory = $true)]$LockedEntry,
        [Parameter(Mandatory = $true)]$Manifest,

        [string]$EvidenceDirectory
    )

    if (Test-Fo4HdrEvidenceDirectory $EvidenceDirectory) { return $true }
    if (Test-Fo4HdrCommonLibObservationEvidence $Manifest) { return $true }
    if (Test-Fo4HdrSemanticEvidence $Manifest) { return $true }
    return [string]$LockedEntry.shaderManifestSha256 -notin @(
        "",
        "UNVALIDATED",
        "FILL_AFTER_CAPTURE"
    )
}

function Get-Fo4HdrRuntimeDefinition {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("1.10.163", "1.10.984", "1.11.240")]
        [string]$Runtime
    )

    $definitions = @{
        "1.10.163" = [pscustomobject]@{
            Runtime = "1.10.163"
            F4SEVersion = "0.6.23"
            RuntimeDll = "f4se_1_10_163.dll"
            AddressLibraryFile = "version-1-10-163-0.bin"
            AllowedPlatforms = @("Steam")
        }
        "1.10.984" = [pscustomobject]@{
            Runtime = "1.10.984"
            F4SEVersion = "0.7.2"
            RuntimeDll = "f4se_1_10_984.dll"
            AddressLibraryFile = "version-1-10-984-0.bin"
            AllowedPlatforms = @("Steam")
        }
        "1.11.240" = [pscustomobject]@{
            Runtime = "1.11.240"
            F4SEVersion = "0.7.9"
            RuntimeDll = "f4se_1_11_240.dll"
            AddressLibraryFile = "version-1-11-240-0.bin"
            AllowedPlatforms = @("Steam")
        }
    }

    return $definitions[$Runtime]
}

function Get-Fo4AddressLibraryPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FileName
    )

    # The override may name another runtime's database; read the requested
    # database from the same Address Library folder.
    if ($env:FO4_ADDRESS_LIBRARY_FILE) {
        $directory = Split-Path -Parent ([IO.Path]::GetFullPath(
            $env:FO4_ADDRESS_LIBRARY_FILE
        ))
        return Join-Path $directory $FileName
    }

    if (-not $env:FO4_ROOT) {
        throw "FO4_ROOT is not set."
    }

    return Join-Path `
        $env:FO4_ROOT `
        ("Data\F4SE\Plugins\" + $FileName)
}

