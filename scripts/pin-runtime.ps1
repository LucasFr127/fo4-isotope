# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExecutablePath,
    [ValidateSet('1.10.163')][string]$Runtime = '1.10.163',
    [string]$ObservationPath = 'build/observed-runtime-1.10.163.json',
    [string]$RecordPath = 'build/pinned-runtime-1.10.163.json',
    [switch]$ApproveObservedRuntime,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))

function Get-BuildRecordPath {
    param([string]$Path)

    $resolved = [IO.Path]::GetFullPath($Path, $root)
    $build = [IO.Path]::Combine($root, 'build') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($build, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Runtime records must remain under the project build directory.'
    }
    return $resolved
}

function Write-PinnedRecord {
    param([string]$Path, [string]$Json)

    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $stream = $null
    $created = $false
    try {
        $stream = [IO.FileStream]::new($temporary, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $created = $true
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Json)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        [IO.File]::Move($temporary, $Path, $true)
    }
    finally {
        if ($stream) { $stream.Dispose() }
        if ($created -and [IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Assert-ReviewedIdentity {
    param([object]$Reviewed, [object]$Current)

    $comparisons = @(
        @($Reviewed.runtime, $Current.runtime),
        @($Reviewed.fileVersion, $Current.fileVersion),
        @($Reviewed.executable.path, $Current.executable.path),
        @($Reviewed.executable.sha256, $Current.executable.sha256)
    )
    foreach ($pair in $comparisons) {
        if (-not [string]::Equals($pair[0], $pair[1], [StringComparison]::Ordinal)) {
            throw 'Executable version/path/hash changed after review; the prior pin is unchanged.'
        }
    }
}

function Assert-PinDestination {
    param([string]$Path, [string]$Schema)

    if (-not [IO.File]::Exists($Path)) { return }
    $existing = [IO.File]::ReadAllText($Path)
    if (-not ($existing | Test-Json -SchemaFile $Schema)) { throw 'Refusing to replace an unrelated record.' }
    if (($existing | ConvertFrom-Json).status -cne 'runtime-pinned-unvalidated') {
        throw 'Pinning must not replace a different record state.'
    }
}

if (-not $ApproveObservedRuntime) {
    throw 'Review the observation, then explicitly pass -ApproveObservedRuntime.'
}
$reviewedPath = Get-BuildRecordPath $ObservationPath
$destination = Get-BuildRecordPath $RecordPath
if ([string]::Equals($reviewedPath, $destination, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The pin record must not overwrite its reviewed observation.'
}
$schema = [IO.Path]::Combine($root, 'schemas/observed-runtime.schema.json')
$json = [IO.File]::ReadAllText($reviewedPath)
if (-not ($json | Test-Json -SchemaFile $schema)) { throw 'Invalid reviewed runtime observation.' }
$reviewed = $json | ConvertFrom-Json
if ($reviewed.status -cne 'observed') { throw 'Approval requires a fresh observed record.' }
$current = & ([IO.Path]::Combine($PSScriptRoot, 'scan-runtime.ps1')) `
    -ExecutablePath $ExecutablePath -Runtime $Runtime -NoWrite -PassThru
if ([string]::Equals($destination, $current.executable.path, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The pin record must not overwrite its executable input.'
}
Assert-ReviewedIdentity $reviewed $current
Assert-PinDestination $destination $schema
# This local record authorizes no loading, validation or release transition.
$reviewed.status = 'runtime-pinned-unvalidated'
$pinnedJson = $reviewed | ConvertTo-Json -Depth 4
if (-not ($pinnedJson | Test-Json -SchemaFile $schema)) { throw 'Invalid pin record.' }
Write-PinnedRecord $destination $pinnedJson
Write-Host "Recorded runtime-pinned-unvalidated: $destination"
if ($PassThru) { return $reviewed }
