# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FixturePath,
    [Parameter(Mandatory)][string]$MismatchFixturePath
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '../..'))
$scan = [IO.Path]::Combine($root, 'scripts/scan-runtime.ps1')
$pin = [IO.Path]::Combine($root, 'scripts/pin-runtime.ps1')
$build = [IO.Path]::Combine($root, 'build') + [IO.Path]::DirectorySeparatorChar
$scratch = $build + 'runtime-pinning-' + [Guid]::NewGuid().ToString('N')

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Case)

    try { & $Action | Out-Null }
    catch { return }
    throw "Runtime pinning accepted a forbidden case: $Case"
}

function Assert-PinUnchanged {
    param([string]$Path, [string]$ExpectedHash)

    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -cne $ExpectedHash) {
        throw 'A rejected pin changed the previous record.'
    }
}

[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $binary = [IO.Path]::Combine($scratch, 'owned fixture.exe')
    $twin = [IO.Path]::Combine($scratch, 'same bytes other path.exe')
    [IO.File]::Copy([IO.Path]::GetFullPath($FixturePath), $binary)
    [IO.File]::Copy($binary, $twin)
    $observationPath = [IO.Path]::Combine($scratch, 'observed.json')
    $recordPath = [IO.Path]::Combine($scratch, 'pinned.json')
    $observed = & $scan -ExecutablePath $binary -OutputPath $observationPath -PassThru
    if ($observed.fileVersion -cne '1.10.163.0') { throw 'Wrong fixture version.' }
    $arguments = @{ ExecutablePath = $binary; ObservationPath = $observationPath; RecordPath = $recordPath }
    Assert-Rejected { & $pin @arguments } 'missing approval'
    if ([IO.File]::Exists($recordPath)) { throw 'Missing approval created a pin.' }
    $pinned = & $pin @arguments -ApproveObservedRuntime -PassThru
    if ($pinned.status -cne 'runtime-pinned-unvalidated') { throw 'Pin implied activation.' }
    $priorHash = (Get-FileHash -LiteralPath $recordPath -Algorithm SHA256).Hash
    Assert-Rejected {
        & $pin -ExecutablePath $twin -ObservationPath $observationPath `
            -RecordPath $recordPath -ApproveObservedRuntime
    } 'different executable path'
    Assert-PinUnchanged $recordPath $priorHash
    [IO.File]::AppendAllText($binary, 'changed hash')
    Assert-Rejected { & $pin @arguments -ApproveObservedRuntime } 'changed executable hash'
    Assert-PinUnchanged $recordPath $priorHash
    [IO.File]::Copy([IO.Path]::GetFullPath($MismatchFixturePath), $binary, $true)
    Assert-Rejected { & $pin @arguments -ApproveObservedRuntime } '1.10.163.1 version mismatch'
    Assert-PinUnchanged $recordPath $priorHash
}
finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    if (-not $resolved.StartsWith($build, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove scratch outside the build directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

Write-Host 'Caller-owned observation/local pin regression tests passed.'
