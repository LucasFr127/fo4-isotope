# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param(
    [ValidateSet('success', 'failure', 'skip', 'hang')][string]$Fixture,
    [string]$PidFile
)

$ErrorActionPreference = 'Stop'
function Start-HangFixture {
    param([string]$PowerShell, [string]$PidFile)

    $start = [Diagnostics.ProcessStartInfo]::new($powerShell)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    foreach ($argument in @('-NoProfile', '-NonInteractive', '-Command',
        'Start-Sleep -Seconds 60')) { $start.ArgumentList.Add($argument) }
    $child = [Diagnostics.Process]::Start($start)
    $self = Get-Process -Id $PID
    try {
        [IO.File]::WriteAllLines($PidFile, [string[]]@(
            "$PID|$($self.StartTime.ToUniversalTime().Ticks)",
            "$($child.Id)|$($child.StartTime.ToUniversalTime().Ticks)"))
    }
    finally { $self.Dispose(); $child.Dispose() }
    Start-Sleep -Seconds 60
}

function Invoke-ProcessFixture {
    param([string]$Fixture, [string]$PowerShell, [string]$PidFile)

    switch ($Fixture) {
        'success' { exit 0 }
        'failure' { exit 23 }
        'skip' { exit 77 }
        'hang' { Start-HangFixture $PowerShell $PidFile; exit 0 }
    }
}

function Get-OwnedFixtureProcess {
    param([string]$Record)

    $fields = $Record.Split('|')
    $process = Get-Process -Id ([int]$fields[0]) -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $null }
    # A recycled PID must never authorize termination of an unrelated process.
    $owned = $false
    try {
        $owned = $process.StartTime.ToUniversalTime().Ticks -eq [long]$fields[1]
        if ($owned) { return $process }
    }
    finally { if (-not $owned) { $process.Dispose() } }
    return $null
}

function Test-ProcessOutcomes {
    param([string]$PowerShell, [string]$Root, [string]$PidPath, [string]$ScriptPath)

    $arguments = @('-NoProfile', '-NonInteractive', '-File', $ScriptPath, '-Fixture')
    foreach ($case in @(
        @{ Name = 'success'; Timeout = 10; Skip = $false; Category = 'passed' },
        @{ Name = 'failure'; Timeout = 10; Skip = $true; Category = 'exitFailure' },
        @{ Name = 'skip'; Timeout = 10; Skip = $false; Category = 'exitFailure' },
        @{ Name = 'skip'; Timeout = 10; Skip = $true; Category = 'skipped' },
        @{ Name = 'hang'; Timeout = 5; Skip = $true; Category = 'timeout' }
    )) {
        $result = Invoke-Fo4HdrTestProcess -FilePath $powerShell `
            -Arguments ($arguments + @($case.Name, '-PidFile', $pidPath)) `
            -WorkingDirectory $root -TimeoutSeconds $case.Timeout -AllowShaderSkip:$case.Skip
        if ($result.category -cne $case.Category -or $result.cleanup -cne 'waitsCompleted') {
            throw "Unexpected $($case.Name) outcome: $($result.category)/$($result.cleanup). $($result.detail)"
        }
    }
}

function Test-FixtureStopped {
    param([string]$PidPath)

    if (-not [IO.File]::Exists($PidPath)) { throw 'Hang fixture did not record its processes.' }
    foreach ($record in [IO.File]::ReadAllLines($PidPath)) {
        $remaining = Get-OwnedFixtureProcess $record
        if ($null -eq $remaining) { continue }
        try {
            if (-not $remaining.WaitForExit(5000)) { throw 'Controlled fixture descendant survived.' }
        }
        finally { $remaining.Dispose() }
    }
}

function Remove-FixtureProcesses {
    param([string]$PidPath)

    if (-not [IO.File]::Exists($PidPath)) { return }
    foreach ($record in [IO.File]::ReadAllLines($PidPath)) {
        $remaining = Get-OwnedFixtureProcess $record
        if ($null -eq $remaining) { continue }
        try { if (-not $remaining.HasExited) { $remaining.Kill($true) } }
        catch [InvalidOperationException] {
            if (-not $remaining.HasExited) { throw }
        }
        finally { $remaining.Dispose() }
    }
    [IO.File]::Delete($PidPath)
}

function Complete-FixtureCleanup {
    param([string]$PidPath, [bool]$TestFailed)

    try { Remove-FixtureProcesses $PidPath }
    catch {
        if (-not $TestFailed) { throw }
        [Console]::Error.WriteLine('Fixture cleanup failed: ' + $_.Exception.Message)
    }
}
$powerShell = (Get-Process -Id $PID).Path
Invoke-ProcessFixture $Fixture $powerShell $PidFile
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. ([IO.Path]::Combine($root, 'scripts/internal/Project.Process.ps1'))
$pidPath = [IO.Path]::Combine([IO.Path]::GetTempPath(),
    'Fallout4HDR-test-process-' + [Guid]::NewGuid().ToString('N') + '.txt')
$testFailed = $false
try {
    Test-ProcessOutcomes $powerShell $root $pidPath $PSCommandPath
    Test-FixtureStopped $pidPath
}
catch { $testFailed = $true; throw }
finally { Complete-FixtureCleanup $pidPath $testFailed }
Write-Host 'PASS controlled process success, failure, explicit77, timeout and fixture descendant checks.'
