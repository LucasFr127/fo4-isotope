# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\..\scripts\internal\NativeSession.Common.ps1")

function Assert-Condition {
    param(
        [Parameter(Mandatory)]
        [bool]$Condition,

        [Parameter(Mandatory)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Start-MonitorProcess {
    param(
        [Parameter(Mandatory)]
        [string]$PowerShellPath,

        [Parameter(Mandatory)]
        [string]$MonitorScript,

        [Parameter(Mandatory)]
        [string]$ProcessName,

        [Parameter(Mandatory)]
        [string]$EvidencePath
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $PowerShellPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @(
        "-NoProfile",
        "-File",
        $MonitorScript,
        "-ProcessName",
        $ProcessName,
        "-StartTimeoutSeconds",
        "10",
        "-EvidencePath",
        $EvidencePath
    )) {
        $null = $startInfo.ArgumentList.Add($argument)
    }
    return [Diagnostics.Process]::Start($startInfo)
}

function Start-TestTarget {
    param(
        [Parameter(Mandatory)]
        [string]$ExecutablePath,

        [Parameter(Mandatory)]
        [int]$ExitCode
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $ExecutablePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    foreach ($argument in @(
        "/d",
        "/c",
        "ping -n 3 127.0.0.1 > nul & exit /b $ExitCode"
    )) {
        $null = $startInfo.ArgumentList.Add($argument)
    }
    return [Diagnostics.Process]::Start($startInfo)
}

function Assert-MonitorRecords {
    param($Records, [string]$ProcessName, [int]$TargetId, [int]$TargetExitCode)

    Assert-Condition ($records.Count -eq 2) "Expected start and stop records."
    Assert-Condition ([int]$records[0].schemaVersion -eq 1) (
        "Unexpected start schema version."
    )
    Assert-Condition ([int]$records[1].schemaVersion -eq 1) (
        "Unexpected stop schema version."
    )
    Assert-Condition ($records[0].event -eq "start") "Missing start record."
    Assert-Condition ($records[1].event -eq "stop") "Missing stop record."
    Assert-Condition ($records[0].process -eq $ProcessName) (
        "Start process name differs."
    )
    Assert-Condition ($records[0].monitoring -is [bool] -and $records[0].monitoring) (
        "Start record did not mark monitoring active."
    )
    Assert-Condition (-not [string]::IsNullOrWhiteSpace(
        $records[0].monitorIdentity)) "Monitor identity was not retained."
    Assert-Condition ($records[0].sessionId -eq $records[1].sessionId) (
        "Session IDs differ."
    )
    Assert-Condition ([int]$records[0].pid -eq $targetId) "Start PID differs."
    Assert-Condition ([int]$records[1].pid -eq $targetId) "Stop PID differs."
    Assert-Condition ([int]$records[1].exitCode -eq $TargetExitCode) (
        "Retained exit code differs."
    )
    Assert-Condition ($records[1].exitCodeAvailable -is [bool] -and $records[1].exitCodeAvailable) (
        "Retained exit code was not marked available."
    )
    Assert-Condition ($records[1].processStopObserved -is [bool] -and $records[1].processStopObserved) (
        "ProcessStop was not retained."
    )
    $startTimestamp = [DateTimeOffset]::Parse($records[0].timestampUtc)
    $stopTimestamp = [DateTimeOffset]::Parse($records[1].timestampUtc)
    $processStart = [DateTimeOffset]::Parse($records[0].processStartUtc)
    Assert-Condition ($stopTimestamp -ge $startTimestamp) (
        "Stop timestamp precedes start timestamp."
    )
    Assert-Condition ($processStart -le $stopTimestamp) (
        "Process start timestamp follows the stop timestamp."
    )
}

function Invoke-MonitorCase {
    param(
        [Parameter(Mandatory)]
        [string]$TemporaryRoot,

        [Parameter(Mandatory)]
        [string]$PowerShellPath,

        [Parameter(Mandatory)]
        [string]$MonitorScript,

        [Parameter(Mandatory)]
        [int]$TargetExitCode
    )

    $token = [Guid]::NewGuid().ToString("N").Substring(0, 8)
    $processName = "Fo4Hdr$token"
    $testExecutable = Join-Path $TemporaryRoot "$processName.exe"
    Copy-Item -LiteralPath $env:ComSpec -Destination $testExecutable
    $evidencePath = Join-Path $TemporaryRoot "$processName.jsonl"
    $monitor = Start-MonitorProcess `
        -PowerShellPath $PowerShellPath `
        -MonitorScript $MonitorScript `
        -ProcessName $processName `
        -EvidencePath $evidencePath
    $readyLine = $monitor.StandardOutput.ReadLine()
    Assert-Condition ($readyLine -like "*waiting_for_start=true*") (
        "Monitor did not become ready: $readyLine"
    )
    $target = Start-TestTarget `
        -ExecutablePath $testExecutable `
        -ExitCode $TargetExitCode
    $targetId = $target.Id
    $target.WaitForExit()
    Assert-Condition ($monitor.WaitForExit(15000)) (
        "Monitor did not finish for target exit code $TargetExitCode."
    )
    $standardOutput = $monitor.StandardOutput.ReadToEnd()
    $standardError = $monitor.StandardError.ReadToEnd()
    Assert-Condition (Test-Path -LiteralPath $evidencePath) (
        "Monitor did not retain evidence: $standardOutput $standardError"
    )
    $records = @(
        Get-Content -LiteralPath $evidencePath |
            ForEach-Object { $_ | ConvertFrom-Json }
    )
    Assert-MonitorRecords $records $processName $targetId $TargetExitCode
    if ($TargetExitCode -eq 0) {
        Assert-Condition ($monitor.ExitCode -eq 0) (
            "Successful target made the monitor fail: $standardError"
        )
    }
    else {
        Assert-Condition ($monitor.ExitCode -ne 0) (
            "Nonzero target did not fail the monitor."
        )
    }
}

function Test-UnavailableExitStatus {
    param(
        [Parameter(Mandatory)]
        [string]$TemporaryRoot
    )

    $readers = @(
        { return $null },
        { throw "simulated exit-code query failure" }
    )
    for ($index = 0; $index -lt $readers.Count; ++$index) {
        $status = Get-Fo4HdrNativeSessionExitStatus `
            -ReadExitCode $readers[$index]
        Assert-Condition (-not $status.Available) (
            "Unavailable exit status was marked available."
        )
        Assert-Condition ($null -eq $status.ExitCode) (
            "Unavailable exit status was coerced to a numeric value."
        )
        $record = New-Fo4HdrNativeSessionStopRecord `
            -SessionId "unavailable-$index" `
            -ProcessIdentity ([pscustomobject]@{ Name = "Fallout4"; Id = 1234 }) `
            -TimestampUtc ([DateTimeOffset]::UtcNow.ToString("O")) `
            -ExitStatus $status
        $path = Join-Path $TemporaryRoot "unavailable-$index.jsonl"
        [IO.File]::WriteAllText(
            $path,
            (($record | ConvertTo-Json -Compress) + "`n"),
            [Text.UTF8Encoding]::new($false)
        )
        $retained = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        Assert-Condition ($null -eq $retained.exitCode) (
            "Unavailable retained exit code was not JSON null."
        )
        Assert-Condition ($retained.exitCodeAvailable -is [bool] -and -not $retained.exitCodeAvailable) (
            "Unavailable retained exit code was marked available."
        )
        Assert-Condition ($retained.processStopObserved -is [bool] -and $retained.processStopObserved) (
            "Unavailable status lost ProcessStop evidence."
        )
        $rejected = $false
        try {
            Assert-Fo4HdrNativeSessionCleanExit `
                -ProcessName "Fallout4" `
                -ExitStatus $status
        }
        catch {
            $rejected =
                $_.Exception.Message -like "*not clean-exit evidence*" -and
                $_.Exception.Message -like "*same Windows user/security context*"
        }
        Assert-Condition $rejected (
            "Unavailable exit status lacked the explicit rejection diagnostic."
        )
    }
}

$temporaryRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    ("Fallout4HDR-monitor-tests-" + [Guid]::NewGuid().ToString("N"))
$resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
$expectedPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$null = New-Item -ItemType Directory -Path $resolvedTemporaryRoot
try {
    $powerShellPath = (Get-Process -Id $PID).Path
    $monitorScript = [IO.Path]::GetFullPath((Join-Path `
        $PSScriptRoot `
        "..\..\scripts\monitor-native-session.ps1"))
    Test-UnavailableExitStatus -TemporaryRoot $resolvedTemporaryRoot
    Invoke-MonitorCase `
        -TemporaryRoot $resolvedTemporaryRoot `
        -PowerShellPath $powerShellPath `
        -MonitorScript $monitorScript `
        -TargetExitCode 0
    Invoke-MonitorCase `
        -TemporaryRoot $resolvedTemporaryRoot `
        -PowerShellPath $powerShellPath `
        -MonitorScript $monitorScript `
        -TargetExitCode 7

    $existing = Join-Path $resolvedTemporaryRoot "existing.jsonl"
    Set-Content -LiteralPath $existing -Value "reserved"
    $rejected = $false
    try {
        & $monitorScript `
            -ProcessName "NoSuchFo4HdrProcess" `
            -StartTimeoutSeconds 1 `
            -EvidencePath $existing
    }
    catch {
        $rejected = $_.Exception.Message -like `
            "*evidence already exists*"
    }
    Assert-Condition $rejected "Existing evidence path was not rejected."
    Assert-Condition ((Get-Content -LiteralPath $existing -Raw).Trim() -eq
        "reserved") "Existing evidence was modified."

    $runningToken = [Guid]::NewGuid().ToString("N").Substring(0, 8)
    $runningName = "Fo4Hdr$runningToken"
    $runningExecutable = Join-Path $resolvedTemporaryRoot "$runningName.exe"
    Copy-Item -LiteralPath $env:ComSpec -Destination $runningExecutable
    $runningTarget = Start-TestTarget `
        -ExecutablePath $runningExecutable `
        -ExitCode 0
    $runningEvidence = Join-Path $resolvedTemporaryRoot "running.jsonl"
    try {
        $runningMonitor = Start-MonitorProcess `
            -PowerShellPath $powerShellPath `
            -MonitorScript $monitorScript `
            -ProcessName $runningName `
            -EvidencePath $runningEvidence
        Assert-Condition ($runningMonitor.WaitForExit(15000)) (
            "Already-running target monitor did not finish."
        )
        $runningOutput = $runningMonitor.StandardOutput.ReadToEnd()
        $runningError = $runningMonitor.StandardError.ReadToEnd()
        Assert-Condition ($runningMonitor.ExitCode -ne 0) (
            "Already-running target was not rejected."
        )
        Assert-Condition (
            "$runningOutput $runningError" -like "*already running*") (
            "Already-running rejection reason was absent."
        )
        Assert-Condition (-not (Test-Path -LiteralPath $runningEvidence)) (
            "Already-running rejection created an evidence artifact."
        )
    }
    finally {
        if (-not $runningTarget.HasExited) {
            $runningTarget.Kill($true)
        }
        $runningTarget.WaitForExit()
        $runningTarget.Dispose()
        if ($null -ne $runningMonitor) {
            $runningMonitor.Dispose()
        }
    }
}
finally {
    if ($resolvedTemporaryRoot.StartsWith(
            $expectedPrefix,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemporaryRoot)) {
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force
    }
}

Write-Host "Native-session monitor tests passed."
