# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot 'scripts/internal/SourceText.ps1')

function Assert-PluginPchBoundary {
    param([string]$Text)
    $header = '\bset_pcxxheader\s*\(\s*(["''])src/plugin/PluginPch\.hpp\1\s*\)'
    $pluginTarget = [regex]::Match($Text, '(?s)\btarget\s*\(\s*(["''])Fallout4HDR\1\s*\).*?\btarget_end\s*\(\s*\)')
    if (-not $pluginTarget.Success -or $pluginTarget.Value -notmatch $header) {
        throw 'The plugin target must own its CommonLib PCH.'
    }
    $otherTargets = $Text.Remove($pluginTarget.Index, $pluginTarget.Length)
    if ($otherTargets -match $header) {
        throw 'The plugin PCH must not propagate to other build targets.'
    }
}

foreach ($quote in @('"', "'")) {
    $valid = "target ( ${quote}Fallout4HDR${quote} )`nset_pcxxheader ( ${quote}src/plugin/PluginPch.hpp${quote} )`ntarget_end ( )"
    Assert-PluginPchBoundary $valid
    foreach ($invalid in @(
        $valid.Replace('set_pcxxheader', 'disabled_set_pcxxheader'),
        "$valid`ntarget('core')`nset_pcxxheader('src/plugin/PluginPch.hpp')`ntarget_end()"
    )) {
        $rejected = $false
        try { Assert-PluginPchBoundary $invalid }
        catch { $rejected = $true }
        if (-not $rejected) { throw 'The PCH boundary check accepted an invalid fixture.' }
    }
}

$xmake = Hide-Fo4HdrNonCodeText -Text (Get-Content -LiteralPath (
    Join-Path $repoRoot 'xmake.lua') -Raw) -Language Lua -PreserveLiterals
Assert-PluginPchBoundary $xmake
Write-Host 'Plugin target PCH isolation check passed.'
