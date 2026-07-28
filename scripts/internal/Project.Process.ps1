# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

Set-StrictMode -Version Latest

function Stop-Fo4HdrTestProcess {
    param([Diagnostics.Process]$Process, [object]$Result)

    try {
        if ($Process.HasExited) { return $true }
        $Process.Kill($true)
        if ($Process.WaitForExit(5000)) { return $true }
        $Result.detail += ' Termination wait expired.'
    }
    catch { $Result.detail += ' Termination failed: ' + $_.Exception.Message }
    return $false
}

function Read-Fo4HdrTestProcessOutput {
    param([Threading.Tasks.Task[]]$Tasks, [object]$Result)

    if ($Tasks.Count -eq 0) { return $true }
    try {
        if (-not [Threading.Tasks.Task]::WaitAll($Tasks, 5000)) {
            $Result.detail += ' Output pipes did not close in the cleanup wait.'
            return $false
        }
        $Result.stdout = $Tasks[0].Result
        $Result.stderr = $Tasks[1].Result
        return $true
    }
    catch { $Result.detail += ' Output drain failed: ' + $_.Exception.Message }
    return $false
}

function Set-Fo4HdrProcessStartInfo {
    param([Diagnostics.Process]$Process, [string]$FilePath,
        [string]$WorkingDirectory, [string[]]$Arguments)

    $Process.StartInfo.FileName = $FilePath
    $Process.StartInfo.WorkingDirectory = $WorkingDirectory
    $Process.StartInfo.UseShellExecute = $false
    $Process.StartInfo.CreateNoWindow = $true
    $Process.StartInfo.RedirectStandardOutput = $true
    $Process.StartInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        if ($null -eq $argument) { throw 'Null process argument.' }
        $Process.StartInfo.ArgumentList.Add($argument)
    }
}

function Set-Fo4HdrTestExitResult {
    param([Diagnostics.Process]$Process, [object]$Result, [bool]$AllowShaderSkip)

    $Result.exitCode = $Process.ExitCode
    $Result.category = if ($Result.exitCode -eq 0) { 'passed' }
        elseif ($AllowShaderSkip -and $Result.exitCode -eq 77) { 'skipped' }
        else { 'exitFailure' }
}

function Complete-Fo4HdrTestProcess {
    param([Diagnostics.Process]$Process, [object[]]$Tasks, [object]$Result)

    # Kill(tree) is best effort while the parent is live. Closed pipes
    # do not prove all descendants exited; an outer job owns containment.
    # Start/Kill scheduling is not bounded by these finite wait windows.
    $stopped = Stop-Fo4HdrTestProcess $Process $Result
    $drained = Read-Fo4HdrTestProcessOutput $Tasks $Result
    $Result.cleanup = if ($stopped -and $drained) { 'waitsCompleted' } else { 'failed' }
    if ($Result.cleanup -eq 'failed' -and $Result.category -in @('passed', 'skipped')) {
        $Result.category = 'cleanupFailure'
    }
}

function Invoke-Fo4HdrTestProcess {
    param(
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$WorkingDirectory,
        [ValidateRange(1, 2147483)][int]$TimeoutSeconds = 120,
        [switch]$AllowShaderSkip
    )

    $result = [pscustomobject]@{
        category = 'startFailure'; exitCode = $null; stdout = ''; stderr = ''
        detail = ''; cleanup = 'notStarted'
    }
    $process = [Diagnostics.Process]::new()
    $started = $false
    $tasks = @()
    try {
        Set-Fo4HdrProcessStartInfo $process $FilePath $WorkingDirectory $Arguments
        $started = $process.Start()
        if (-not $started) { throw 'Process.Start returned false.' }
        $tasks = @($process.StandardOutput.ReadToEndAsync(),
            $process.StandardError.ReadToEndAsync())
        if ($process.WaitForExit($TimeoutSeconds * 1000)) {
            Set-Fo4HdrTestExitResult $process $result $AllowShaderSkip
        }
        else {
            $result.category = 'timeout'
            $result.detail = "Execution wait expired after $TimeoutSeconds seconds."
        }
    }
    catch {
        if ($started) { $result.category = 'executionFailure' }
        $result.detail += $_.Exception.Message
    }
    finally {
        try {
            if ($started) {
                Complete-Fo4HdrTestProcess $process $tasks $result
            }
        }
        finally { $process.Dispose() }
    }
    return $result
}
