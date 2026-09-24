# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()]
    [string]$ProcessName = "Fallout4",

    [ValidateRange(1, 3600)]
    [int]$StartTimeoutSeconds = 300,

    [string]$EvidencePath = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/NativeSession.Common.ps1")
$resolvedEvidencePath = $null
if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
    $resolvedEvidencePath = [IO.Path]::GetFullPath($EvidencePath)
    if (Test-Path -LiteralPath $resolvedEvidencePath) {
        throw "Native-session evidence already exists: $resolvedEvidencePath"
    }
}
$existingTarget = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -ne $existingTarget) {
    throw "Process '$ProcessName' is already running; start the monitor before launch."
}
Write-Host (
    "native_session process={0} waiting_for_start=true timeout_seconds={1}" -f
    $ProcessName,
    $StartTimeoutSeconds
)
$deadline = [DateTimeOffset]::UtcNow.AddSeconds($StartTimeoutSeconds)
$target = $null

while ($null -eq $target -and [DateTimeOffset]::UtcNow -lt $deadline) {
    $target = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
    if ($null -eq $target) {
        Start-Sleep -Milliseconds 250
    }
}

if ($null -eq $target) {
    throw "Process '$ProcessName' did not start within $StartTimeoutSeconds seconds."
}

$targetName = $target.ProcessName
$targetId = $target.Id
$startUtc = $target.StartTime.ToUniversalTime().ToString("O")
$sessionId = [Guid]::NewGuid().ToString("D")
$monitorIdentity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
if ($null -eq $resolvedEvidencePath) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $startToken = $target.StartTime.ToUniversalTime().ToString(
        "yyyyMMddTHHmmssfffZ"
    )
    $EvidencePath = Join-Path `
        (Join-Path $repoRoot "build\diagnostics") `
        ("native-session-{0}-{1}-{2}.jsonl" -f `
            $targetName, `
            $targetId, `
            $startToken)
    $resolvedEvidencePath = [IO.Path]::GetFullPath($EvidencePath)
}
if (Test-Path -LiteralPath $resolvedEvidencePath) {
    throw "Native-session evidence already exists: $resolvedEvidencePath"
}
$evidenceDirectory = Split-Path -Parent $resolvedEvidencePath
if (-not [string]::IsNullOrEmpty($evidenceDirectory)) {
    $null = New-Item -ItemType Directory -Force -Path $evidenceDirectory
}
$encoding = [Text.UTF8Encoding]::new($false)
$stream = [IO.FileStream]::new(
    $resolvedEvidencePath,
    [IO.FileMode]::CreateNew,
    [IO.FileAccess]::Write,
    [IO.FileShare]::Read
)
$writer = [IO.StreamWriter]::new($stream, $encoding)
$writer.AutoFlush = $true

function Write-NativeSessionEvidence {
    param(
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Record,

        [Parameter(Mandatory)]
        [string]$Message
    )

    $writer.WriteLine(($Record | ConvertTo-Json -Compress))
    Write-Host $Message
}

# Open and retain the process handle before exit. A Process object discovered
# through Get-Process cannot reliably expose ExitCode afterward otherwise.
$null = $target.Handle
try {
    Write-NativeSessionEvidence -Record ([ordered]@{
        schemaVersion = 1
        event = "start"
        sessionId = $sessionId
        process = $targetName
        pid = $targetId
        timestampUtc = [DateTimeOffset]::UtcNow.ToString("O")
        processStartUtc = $startUtc
        monitorIdentity = $monitorIdentity
        monitoring = $true
    }) -Message ((
        "native_session process={0} pid={1} start_utc={2} monitoring=true " +
        "evidence_path={3}"
    ) -f $targetName, $targetId, $startUtc, $resolvedEvidencePath)

    $target.WaitForExit()
    $target.Refresh()
    $stopUtc = [DateTimeOffset]::UtcNow.ToString("O")
    $exitStatus = Get-Fo4HdrNativeSessionExitStatus -ReadExitCode {
        $target.ExitCode
    }
    Write-NativeSessionEvidence -Record (
        New-Fo4HdrNativeSessionStopRecord `
            -SessionId $sessionId `
            -ProcessIdentity ([pscustomobject]@{ Name = $targetName; Id = $targetId }) `
            -TimestampUtc $stopUtc `
            -ExitStatus $exitStatus
    ) -Message ((
        "native_session process={0} pid={1} stop_utc={2} exit_code={3} " +
        "exit_code_available={4} process_stop_observed=true " +
        "evidence_path={5}"
    ) -f $targetName, $targetId, $stopUtc, $exitStatus.ExitCode,
        $exitStatus.Available.ToString().ToLowerInvariant(),
        $resolvedEvidencePath)
}
finally {
    $writer.Dispose()
}

Assert-Fo4HdrNativeSessionCleanExit `
    -ProcessName $ProcessName `
    -ExitStatus $exitStatus
