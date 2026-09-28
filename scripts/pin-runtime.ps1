# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Steam", "GOG")]
    [string]$Platform,

    [string]$ObservationPath,

    [Parameter(Mandatory = $true)]
    [switch]$ApproveObservedRuntime
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

if (-not $ApproveObservedRuntime) {
    throw "Pass -ApproveObservedRuntime only after reviewing the observation file."
}

$repoRoot = Get-Fo4HdrRepoRoot
if (-not $ObservationPath) {
    $ObservationPath =
        Join-Path $repoRoot "build\observed-runtime-$Runtime.json"
}

if (-not (Test-Path $ObservationPath -PathType Leaf)) {
    throw (
        "Reviewed observation file not found: $ObservationPath. " +
        "Run scripts/scan-runtime.ps1 first."
    )
}

$observationText = Get-Content $ObservationPath -Raw
$observationSchema =
    Join-Path $repoRoot "schemas\observed-runtime.schema.json"
if (-not ($observationText | Test-Json -SchemaFile $observationSchema)) {
    throw "Observed runtime file does not match its schema."
}

$observation = $observationText | ConvertFrom-Json
if ($observation.runtime -ne $Runtime) {
    throw "Observation runtime does not match -Runtime."
}
if ($observation.platform -ne $Platform.ToLowerInvariant()) {
    throw "Observation platform does not match -Platform."
}

$current = & (Join-Path $PSScriptRoot "scan-runtime.ps1") `
    -Runtime $Runtime `
    -Platform $Platform `
    -PassThru `
    -NoWrite

$comparisons = @(
    @{ Name = "file version"; Reviewed = $observation.fileVersion; Current = $current.fileVersion },
    @{ Name = "FO4 root"; Reviewed = $observation.fo4Root; Current = $current.fo4Root },
    @{ Name = "Fallout4.exe path"; Reviewed = $observation.fallout4Exe.path; Current = $current.fallout4Exe.path },
    @{ Name = "Fallout4.exe hash"; Reviewed = $observation.fallout4Exe.sha256; Current = $current.fallout4Exe.sha256 },
    @{ Name = "F4SE loader path"; Reviewed = $observation.f4seLoader.path; Current = $current.f4seLoader.path },
    @{ Name = "F4SE loader hash"; Reviewed = $observation.f4seLoader.sha256; Current = $current.f4seLoader.sha256 },
    @{ Name = "F4SE runtime path"; Reviewed = $observation.f4seRuntime.path; Current = $current.f4seRuntime.path },
    @{ Name = "F4SE runtime version"; Reviewed = $observation.f4seRuntime.version; Current = $current.f4seRuntime.version },
    @{ Name = "F4SE runtime hash"; Reviewed = $observation.f4seRuntime.sha256; Current = $current.f4seRuntime.sha256 },
    @{ Name = "Address Library path"; Reviewed = $observation.addressLibrary.path; Current = $current.addressLibrary.path },
    @{ Name = "Address Library file"; Reviewed = $observation.addressLibrary.file; Current = $current.addressLibrary.file },
    @{ Name = "Address Library hash"; Reviewed = $observation.addressLibrary.sha256; Current = $current.addressLibrary.sha256 }
)

foreach ($comparison in $comparisons) {
    if (-not [string]::Equals(
        [string]$comparison.Reviewed,
        [string]$comparison.Current,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw (
            $comparison.Name +
            " changed after the observation was reviewed. " +
            "Run scan-runtime.ps1 again and review the new file."
        )
    }
}

$lockPath = Join-Path $repoRoot "versions.lock.json"
$lock = Get-Content $lockPath -Raw | ConvertFrom-Json
$entry = $lock.runtimes.$Runtime

$manifestPath =
    Join-Path $repoRoot "runtime\$Runtime\manifest.json"
$manifest =
    Get-Content $manifestPath -Raw | ConvertFrom-Json

if ((Test-Fo4HdrRuntimeIdentityChanged `
        -LockedEntry $entry `
        -Observation $observation) -and
    (Test-Fo4HdrIdentityBoundEvidencePresent `
        -LockedEntry $entry `
        -Manifest $manifest `
        -EvidenceDirectory (Join-Path `
            $repoRoot `
            "runtime\$Runtime\validation"))) {
    throw (
        "The reviewed binary identity differs from an identity with " +
        "retained validation evidence. Use an explicitly scoped evidence-" +
        "reset/promotion procedure; pin-runtime.ps1 will not rebind stale " +
        "manual, CommonLib, shader, semantic, or resource evidence."
    )
}

Write-Host "Approving reviewed runtime observation:"
$observation | ConvertTo-Json -Depth 16 | Write-Host

$entry.status = "runtime-pinned-unvalidated"
$entry.platform = $observation.platform
$entry.f4seVersion = $observation.f4seRuntime.version
$entry.exeSha256 = $observation.fallout4Exe.sha256
$entry.f4seLoaderSha256 = $observation.f4seLoader.sha256
$entry.f4seRuntimeDllSha256 =
    $observation.f4seRuntime.sha256
$entry.addressLibraryFile =
    $observation.addressLibrary.file
$entry.addressLibrarySha256 =
    $observation.addressLibrary.sha256
$entry.observedAtUtc =
    $observation.observedAtUtc

$manifest.runtime.platform = $observation.platform
$manifest.runtime.exeSha256 =
    $observation.fallout4Exe.sha256
$manifest.runtime.addressLibraryFile =
    $observation.addressLibrary.file
$manifest.runtime.status =
    "runtime-pinned-unvalidated"

Write-Fo4HdrJsonFile `
    -Value $manifest `
    -Path $manifestPath `
    -SchemaPath (Join-Path $repoRoot "schemas\runtime-manifest.schema.json")

$entry | Add-Member `
    -NotePropertyName "runtimeManifestCanonicalSha256" `
    -NotePropertyValue (
        Get-Fo4HdrCanonicalTextSha256 $manifestPath
    ) `
    -Force

Write-Fo4HdrJsonFile `
    -Value $lock `
    -Path $lockPath `
    -SchemaPath (Join-Path $repoRoot "schemas\versions-lock.schema.json")

if ($Runtime -eq "1.10.163") {
    & (Join-Path $PSScriptRoot "generate-runtime-identity.ps1") `
        -Runtime $Runtime
}

& (Join-Path $PSScriptRoot "validate-manifests.ps1")

Write-Host "Pinned Fallout 4 $Runtime ($Platform) for scoped validation."
Write-Host (
    "This records only the reviewed executable/runtime identity. " +
    "Shader, target, and manual HDR validation remain incomplete."
)
