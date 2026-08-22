# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param([ValidateSet('1.10.163')][string]$Runtime = '1.10.163')

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))

function Copy-AvailableRecord {
    param([string]$RelativePath, [string]$Destination)

    $source = [IO.Path]::Combine($root, $RelativePath)
    if (-not [IO.File]::Exists($source)) {
        Write-Host "Optional diagnostic input absent: $RelativePath"
        return
    }
    Copy-Item -LiteralPath $source -Destination $Destination
}

function Write-GitSnapshot {
    param([string[]]$Arguments, [string]$Path)

    $result = & git -C $root @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Git snapshot failed: $Arguments" }
    $result | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

if ([string]::IsNullOrWhiteSpace($env:FO4_CAPTURE_ROOT)) {
    throw 'FO4_CAPTURE_ROOT is not set.'
}
$captureRoot = [IO.Path]::GetFullPath($env:FO4_CAPTURE_ROOT)
$token = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'),
    [Guid]::NewGuid().ToString('N')
$destination = [IO.Path]::Combine($captureRoot, 'diagnostics', $token)
# A fresh collection must not overwrite a prior session's evidence.
New-Item -ItemType Directory -Path $destination -ErrorAction Stop | Out-Null

if ($env:FO4_LOG_ROOT -and (Test-Path -LiteralPath $env:FO4_LOG_ROOT)) {
    $logRoot = [IO.Path]::GetFullPath($env:FO4_LOG_ROOT).TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    if ($destination.StartsWith($logRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The collection destination must not be inside its log input.'
    }
    $logs = Join-Path $destination 'logs'
    New-Item -ItemType Directory -Path $logs | Out-Null
    Get-ChildItem -LiteralPath $env:FO4_LOG_ROOT | Copy-Item -Destination $logs -Recurse
}
Copy-Item -LiteralPath (Join-Path $root 'xmake.lua') -Destination $destination
Copy-AvailableRecord "build/observed-runtime-$Runtime.json" $destination
Copy-AvailableRecord "build/pinned-runtime-$Runtime.json" $destination
[ordered]@{ runtime = $Runtime; powerShellVersion = $PSVersionTable.PSVersion.ToString() } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'collector.json') -Encoding utf8NoBOM
Write-GitSnapshot @('status', '--short') (Join-Path $destination 'git-status.txt')
Write-GitSnapshot @('submodule', 'status', '--recursive') (Join-Path $destination 'submodules.txt')
Write-Host "Diagnostics collected at: $destination"
