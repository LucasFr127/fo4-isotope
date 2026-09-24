# SPDX-License-Identifier: GPL-3.0-or-later

Set-StrictMode -Version Latest

function Invoke-Fo4HdrTestProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)]
        [ValidateRange(1, 2147483)][int]$TimeoutSeconds,
        [switch]$AllowSkip
    )

    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = $FilePath
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.WorkingDirectory = $WorkingDirectory
    foreach ($argument in $Arguments) {
        $process.StartInfo.ArgumentList.Add($argument)
    }
    $label = "$FilePath $($Arguments -join ' ')"
    $started = $false
    $outputTasks = @()
    try {
        $started = $process.Start()
        if (-not $started) { throw "Unable to start test: $label" }
        # Drain both pipes concurrently; publish through PowerShell so logs can redirect them.
        $outputTasks = @(
            $process.StandardOutput.ReadToEndAsync(),
            $process.StandardError.ReadToEndAsync()
        )
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            throw "Test timed out after $TimeoutSeconds seconds: $label"
        }
        Assert-Fo4HdrTestExit $process $AllowSkip $label
    }
    finally {
        try {
            if ($started) { Stop-Fo4HdrTestProcess $process $label }
            Write-Fo4HdrTestProcessOutput $outputTasks $label
        }
        finally { $process.Dispose() }
    }
}

function Stop-Fo4HdrTestProcess {
    param([Diagnostics.Process]$Process, [string]$Label)

    if (-not $process.HasExited) {
        try { $process.Kill($true) }
        catch [InvalidOperationException] {
            if (-not $process.HasExited) { throw }
        }
        if (-not $process.WaitForExit(5000)) {
            throw "Test process did not exit after termination: $label"
        }
    }
}

function Write-Fo4HdrTestProcessOutput {
    param([object[]]$OutputTasks, [string]$Label)

    if ($outputTasks.Count -and
        -not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]$outputTasks, 5000)) {
        throw "Test output did not close after termination: $label"
    }
    if ($outputTasks.Count) {
        if ($outputTasks[0].Result.Length) { Write-Output $outputTasks[0].Result }
        if ($outputTasks[1].Result.Length) { Write-Host -NoNewline $outputTasks[1].Result }
    }
}

function Assert-Fo4HdrTestExit {
    param([Diagnostics.Process]$Process, [bool]$AllowSkip, [string]$Label)
    if ($AllowSkip -and $process.ExitCode -eq 77) {
        Write-Host "SKIP: $label"
    } elseif ($process.ExitCode -ne 0) {
        throw "Test failed with exit code $($process.ExitCode): $label"
    }
}
