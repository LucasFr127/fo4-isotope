# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
& (Join-Path $root 'scripts/audit-f4se-project.ps1') -Root $root
$recipe = [IO.File]::ReadAllText((Join-Path $root 'xmake.lua'))
$plugin = [regex]::Match($recipe,
    '(?s)target\("Fallout4HDR"\).*?target_end\(\)')
if (!$plugin.Success -or $plugin.Value -notmatch 'add_deps\("commonlibf4"') {
    throw 'The plugin target must own CommonLib linkage.'
}
$otherTargets = $recipe.Remove($plugin.Index, $plugin.Length)
if ($otherTargets -match 'add_deps\([^\r\n]*"commonlibf4"') {
    throw 'CommonLib linkage must not propagate to core or test targets.'
}
if ($recipe -match 'set_pcxxheader\([^\r\n]*src/plugin/') {
    throw 'A plugin PCH requires an explicit plugin-only source boundary.'
}
Write-Host 'PASS plugin-only CommonLib linkage and current PCH boundary'
