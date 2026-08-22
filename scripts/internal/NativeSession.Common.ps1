# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

function Get-Fo4HdrNativeSessionExitStatus {
    param(
        [Parameter(Mandatory)]
        [scriptblock]$ReadExitCode
    )

    try {
        $candidate = & $ReadExitCode
        if ($null -ne $candidate) {
            return [pscustomobject]@{
                ExitCode = [int]$candidate
                Available = $true
            }
        }
    }
    catch {
        # The caller persists an unavailable result and rejects the run.
    }
    return [pscustomobject]@{
        ExitCode = $null
        Available = $false
    }
}

function New-Fo4HdrNativeSessionStopRecord {
    param(
        [Parameter(Mandatory)]
        [string]$SessionId,

        [Parameter(Mandatory)]
        [pscustomobject]$ProcessIdentity,

        [Parameter(Mandatory)]
        [string]$TimestampUtc,

        [Parameter(Mandatory)]
        [pscustomobject]$ExitStatus
    )

    return [ordered]@{
        schemaVersion = 1
        event = "stop"
        sessionId = $SessionId
        process = [string]$ProcessIdentity.Name
        pid = [int]$ProcessIdentity.Id
        timestampUtc = $TimestampUtc
        exitCode = $ExitStatus.ExitCode
        exitCodeAvailable = [bool]$ExitStatus.Available
        processStopObserved = $true
    }
}

function Assert-Fo4HdrNativeSessionCleanExit {
    param(
        [Parameter(Mandatory)]
        [string]$ProcessName,

        [Parameter(Mandatory)]
        [pscustomobject]$ExitStatus
    )

    if (-not $ExitStatus.Available) {
        throw (
            "Process '$ProcessName' stopped, but its exit code was " +
            "unavailable. Run the monitor from the same Windows " +
            "user/security context as the game; this artifact is not " +
            "clean-exit evidence."
        )
    }
    if ([int]$ExitStatus.ExitCode -ne 0) {
        throw (
            "Process '$ProcessName' exited with code " +
            "$($ExitStatus.ExitCode)."
        )
    }
}
