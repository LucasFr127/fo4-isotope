# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$extensions = @('.hpp', '.cpp', '.hlsl', '.hlsli', '.ps1', '.lua', '.md', '.ini')
$files = @(Get-ChildItem -LiteralPath $root -File)
foreach ($directory in @('src', 'include', 'tests', 'scripts', 'shaders', 'config')) {
    $files += Get-ChildItem -LiteralPath (Join-Path $root $directory) -File -Recurse
}
foreach ($file in $files) {
    if ($file.Extension -notin $extensions) { continue }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    $text = $utf8.GetString($bytes)
    if ($text.IndexOf([char]0) -ge 0) { throw "NUL in maintained source: $($file.Name)" }
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) {
        throw "UTF-8 BOM in maintained source: $($file.Name)"
    }
}
Write-Host 'PASS maintained strict UTF-8 source text without BOM or NUL'
