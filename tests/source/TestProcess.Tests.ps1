# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param(
    [ValidateSet('success', 'failure', 'skip', 'hang', 'diagnostics')][string]$Fixture,
    [string]$PidFile,
    [string]$Message
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
    param([string]$Fixture, [string]$PowerShell, [string]$PidFile, [string]$Message)

    switch ($Fixture) {
        'diagnostics' {
            [Console]::Out.WriteLine("stdout: $Message")
            [Console]::Error.WriteLine("stderr: $Message")
            exit 23
        }
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

function Assert-ProcessValue {
    param($Actual, $Expected, [string]$Message)

    if ($Actual -cne $Expected) { throw $Message }
}

function Test-ProcessDiagnostics {
    param([string]$PowerShell, [string]$Root, [string]$ScriptPath)

    $message = 'value with spaces "quoted" tail'
    $result = Invoke-Fo4HdrTestProcess -FilePath $PowerShell -WorkingDirectory $Root `
        -Arguments @('-NoProfile', '-NonInteractive', '-File', $ScriptPath,
            '-Fixture', 'diagnostics', '-Message', $message) -TimeoutSeconds 10 -AllowShaderSkip
    Assert-ProcessValue $result.category 'exitFailure' 'Diagnostic fixture was classified as success or skip.'
    Assert-ProcessValue $result.exitCode 23 'Diagnostic fixture did not preserve its nonzero exit.'
    Assert-ProcessValue $result.cleanup 'waitsCompleted' 'Diagnostic fixture cleanup failed.'
    Assert-ProcessValue $result.stdout "stdout: $message$([Environment]::NewLine)" 'Spaced stdout argument was not preserved.'
    Assert-ProcessValue $result.stderr "stderr: $message$([Environment]::NewLine)" 'Spaced stderr argument was not preserved.'
}

function Test-InvalidProcessFlag {
    param([string]$PowerShell, [string]$Root, [string]$ScriptPath)

    $result = Invoke-Fo4HdrTestProcess -FilePath $PowerShell -WorkingDirectory $Root `
        -Arguments @('-NoProfile', '-NonInteractive', '-File', $ScriptPath,
            '-Fixture', 'success', '-UnknownFixtureFlag') -TimeoutSeconds 10 -AllowShaderSkip
    Assert-ProcessValue $result.category 'exitFailure' 'Invalid child flag was accepted or classified as a skip.'
    Assert-ProcessValue $result.cleanup 'waitsCompleted' 'Invalid child flag cleanup failed.'
    if ($result.exitCode -eq 0) { throw 'Invalid child flag reported a successful exit.' }
    if ([string]::IsNullOrWhiteSpace($result.stderr)) { throw 'Invalid child flag lost its diagnostic.' }
}

function Test-InvalidProcessTimeout {
    param([string]$PowerShell, [string]$Root)

    try {
        $null = Invoke-Fo4HdrTestProcess -FilePath $PowerShell -WorkingDirectory $Root -TimeoutSeconds 0
    }
    catch [Management.Automation.ParameterBindingValidationException] { return }
    throw 'Invalid timeout was not rejected before process start.'
}

function Test-ProcessStartFailure {
    param([string]$Root)

    $missing = [IO.Path]::Combine([IO.Path]::GetTempPath(),
        'Fallout4HDR-missing-' + [Guid]::NewGuid().ToString('N') + '.exe')
    $result = Invoke-Fo4HdrTestProcess -FilePath $missing -WorkingDirectory $Root -TimeoutSeconds 10
    Assert-ProcessValue $result.category 'startFailure' 'Missing executable did not fail at start.'
    Assert-ProcessValue $result.exitCode $null 'Unstarted process reported an exit code.'
    Assert-ProcessValue $result.cleanup 'notStarted' 'Unstarted process reported started cleanup.'
    Assert-ProcessValue $result.stdout '' 'Unstarted process reported stdout.'
    Assert-ProcessValue $result.stderr '' 'Unstarted process reported stderr.'
    if ([string]::IsNullOrWhiteSpace($result.detail)) { throw 'Start failure lost its diagnostic.' }
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
Invoke-ProcessFixture $Fixture $powerShell $PidFile $Message
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. ([IO.Path]::Combine($root, 'scripts/internal/Project.Process.ps1'))
$pidPath = [IO.Path]::Combine([IO.Path]::GetTempPath(),
    'Fallout4HDR-test-process-' + [Guid]::NewGuid().ToString('N') + '.txt')
$testFailed = $false
try {
    Test-ProcessOutcomes $powerShell $root $pidPath $PSCommandPath
    Test-FixtureStopped $pidPath
    Test-ProcessDiagnostics $powerShell $root $PSCommandPath
    Test-InvalidProcessFlag $powerShell $root $PSCommandPath
    Test-InvalidProcessTimeout $powerShell $root
    Test-ProcessStartFailure $root
}
catch { $testFailed = $true; throw }
finally { Complete-FixtureCleanup $pidPath $testFailed }
Write-Host 'PASS controlled process outcomes, descendants, spaced arguments, both streams and rejected inputs.'
