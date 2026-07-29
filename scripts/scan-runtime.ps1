# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExecutablePath,
    [ValidateSet('1.10.163')][string]$Runtime = '1.10.163',
    [string]$OutputPath = 'build/observed-runtime-1.10.163.json',
    [switch]$NoWrite,
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

function Write-Observation {
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

function Assert-ObservationDestination {
    param([string]$Path, [string]$Schema)

    if (-not [IO.File]::Exists($Path)) { return }
    $existing = [IO.File]::ReadAllText($Path)
    if (-not ($existing | Test-Json -SchemaFile $Schema)) { throw 'Refusing to replace an unrelated record.' }
    if (($existing | ConvertFrom-Json).status -cne 'observed') {
        throw 'Scanning must not replace an approved pin record.'
    }
}

$path = [IO.Path]::GetFullPath($ExecutablePath)
# The read handle denies write/delete while version and hash are observed.
$binary = [IO.FileStream]::new($path, [IO.FileMode]::Open,
    [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    $fileVersion = '{0}.{1}.{2}.{3}' -f $version.FileMajorPart, $version.FileMinorPart,
        $version.FileBuildPart, $version.FilePrivatePart
    if ($fileVersion -cne '1.10.163.0') {
        throw "Executable fixed file version is not exactly 1.10.163.0: $fileVersion"
    }
    $hash = (Get-FileHash -InputStream $binary -Algorithm SHA256).Hash.ToLowerInvariant()
}
finally { $binary.Dispose() }

$observation = [ordered]@{
    schemaVersion = 1
    observedAtUtc = [DateTime]::UtcNow.ToString('o')
    runtime = $Runtime
    status = 'observed'
    fileVersion = $fileVersion
    executable = [ordered]@{ path = $path; sha256 = $hash }
}
$json = $observation | ConvertTo-Json -Depth 4
$schema = [IO.Path]::Combine($root, 'schemas/observed-runtime.schema.json')
if (-not ($json | Test-Json -SchemaFile $schema)) { throw 'Invalid runtime observation.' }
if (-not $NoWrite) {
    $destination = Get-BuildRecordPath $OutputPath
    if ([string]::Equals($destination, $path, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The observation must not overwrite its executable input.'
    }
    Assert-ObservationDestination $destination $schema
    Write-Observation $destination $json
    Write-Host "Observed caller-owned executable: $destination"
}
if ($PassThru) { return [pscustomobject]$observation }
