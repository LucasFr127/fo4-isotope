# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet('success', 'failure', 'skip', 'hang')][string]$Fixture,
    [string]$PidFile
)

$ErrorActionPreference = 'Stop'
$powerShell = (Get-Process -Id $PID).Path
if ($Fixture) {
    [Console]::Out.WriteLine('Test fixture stdout.')
    [Console]::Error.WriteLine('Test fixture stderr.')
}
if ($Fixture -eq 'success') {
    [Console]::Out.WriteLine(('o' * 131072))
    [Console]::Error.WriteLine(('e' * 131072))
    exit 0
}
if ($Fixture -eq 'failure') { exit 23 }
if ($Fixture -eq 'skip') { exit 77 }
if ($Fixture -eq 'hang') {
    $startInfo = [Diagnostics.ProcessStartInfo]::new($powerShell)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    foreach ($argument in @('-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Seconds 60')) {
        $startInfo.ArgumentList.Add($argument)
    }
    $child = [Diagnostics.Process]::Start($startInfo)
    [IO.File]::WriteAllLines($PidFile, [string[]]@($PID, $child.Id))
    Start-Sleep -Seconds 60
    exit 0
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot 'scripts/internal/Project.Common.ps1')
$pidPath = Join-Path ([IO.Path]::GetTempPath()) ("FO4Isotope test process $([Guid]::NewGuid().ToString('N')).txt")
$logPath = $pidPath + '.log'
$arguments = @('-NoProfile', '-NonInteractive', '-File', $PSCommandPath, '-Fixture')
try {
    foreach ($case in @(
        @{ Name = 'success'; Timeout = 10; AllowSkip = $false; Expected = '' },
        @{ Name = 'failure'; Timeout = 10; AllowSkip = $true; Expected = 'Test failed with exit code 23:*' },
        @{ Name = 'skip'; Timeout = 10; AllowSkip = $false; Expected = 'Test failed with exit code 77:*' },
        @{ Name = 'skip'; Timeout = 10; AllowSkip = $true; Expected = '' },
        @{ Name = 'hang'; Timeout = 5; AllowSkip = $false; Expected = 'Test timed out after 5 seconds:*' }
    )) {
        $message = ''
        try {
            Invoke-Fo4HdrTestProcess -FilePath $powerShell `
                -Arguments ($arguments + @($case.Name, '-PidFile', $pidPath)) `
                -WorkingDirectory $repoRoot -TimeoutSeconds $case.Timeout `
                -AllowSkip:$case.AllowSkip *> $logPath
        }
        catch { $message = $_.Exception.Message }
        if ($message -notlike $case.Expected) {
            throw "Unexpected $($case.Name) result: '$message'"
        }
        $log = Get-Content -LiteralPath $logPath -Raw
        foreach ($marker in @('Test fixture stdout.', 'Test fixture stderr.')) {
            if (-not $log.Contains($marker)) { throw "$($case.Name) lost redirected output: $marker" }
        }
        if ($case.Name -eq 'skip' -and $case.AllowSkip -and -not $log.Contains('SKIP:')) {
            throw 'Skipped test was not reported.'
        }
        if ($case.Name -eq 'success' -and
            (-not $log.Contains(('o' * 131072)) -or -not $log.Contains(('e' * 131072)))) {
            throw 'Large redirected test output was truncated.'
        }
    }

    if (-not (Test-Path -LiteralPath $pidPath)) { throw 'Hang fixture did not start.' }
    foreach ($processId in Get-Content -LiteralPath $pidPath) {
        $remaining = Get-Process -Id ([int]$processId) -ErrorAction SilentlyContinue
        if ($null -ne $remaining) {
            try {
                if (-not $remaining.WaitForExit(5000)) {
                    throw "Timeout left fixture process $processId running."
                }
            }
            finally { $remaining.Dispose() }
        }
    }
}
finally {
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath }
    if (Test-Path -LiteralPath $pidPath) {
        foreach ($processId in Get-Content -LiteralPath $pidPath) {
            $remaining = Get-Process -Id ([int]$processId) -ErrorAction SilentlyContinue
            if ($null -ne $remaining) {
                try { $remaining.Kill($true) }
                catch [InvalidOperationException] {
                    if (-not $remaining.HasExited) { throw }
                }
                finally { $remaining.Dispose() }
            }
        }
        Remove-Item -LiteralPath $pidPath
    }
}
Write-Host 'Test process success, failure, skip, timeout, and descendant cleanup checks passed.'
