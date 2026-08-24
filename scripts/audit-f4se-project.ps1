# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$Root = (Split-Path $PSScriptRoot -Parent))

$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')

function Assert-EngineBoundary {
    param([string]$Path, [string]$Text)
    if ($Path.StartsWith('src/game/')) { return }
    if ($Text -match '\bRE\s*::|(?m)^\s*#\s*include\s*["<]RE/') {
        throw "Engine access outside the game owner: $Path"
    }
    if ($Path -ceq 'src/plugin/PluginMain.cpp') { return }
    if ($Text -match '\b(?:F4SE|REL)\s*::|(?m)^\s*#\s*include\s*["<](?:F4SE|REL)/') {
        throw "F4SE/CommonLib access outside the game/bootstrap boundary: $Path"
    }
}

# This bounded source scan is a review lead; it cannot establish runtime safety.
foreach ($directory in @('src', 'include')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Root $directory) -File -Recurse) {
        if ($file.Extension -notin @('.hpp', '.cpp')) { continue }
        $relative = $file.FullName.Substring($Root.Length + 1).Replace('\', '/')
        Assert-EngineBoundary $relative ([IO.File]::ReadAllText($file.FullName))
    }
}
Write-Host 'PASS maintained engine/F4SE source boundaries (heuristic only)'
