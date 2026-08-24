# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$paths = @{}
$files = @(foreach ($directory in @('src', 'include', 'tests')) {
    Get-ChildItem -LiteralPath (Join-Path $root $directory) -File -Recurse |
        Where-Object { $_.Extension -in @('.hpp', '.cpp') }
})
foreach ($file in $files) {
    $path = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
    if ($paths.ContainsKey($path)) { throw "Case-insensitive source collision: $path" }
    $paths[$path] = $path
    if ($file.Name -cne 'main.cpp' -and $file.Name -cnotmatch '^[A-Z][A-Za-z0-9]*\.(hpp|cpp)$') {
        throw "Native source filename must use PascalCase: $path"
    }
}
$includeRoots = @('include', 'src/runtime', 'src/game', 'src/graphics', 'src/ui',
    'tests/unit', 'tests/support', 'tests/shader')
foreach ($file in $files) {
    $path = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
    $text = [IO.File]::ReadAllText($file.FullName)
    foreach ($match in [regex]::Matches($text, '(?m)^\s*#\s*include\s*"([^"\r\n]+)"')) {
        $include = $match.Groups[1].Value
        $candidates = @($file.DirectoryName) + @($includeRoots | ForEach-Object { Join-Path $root $_ })
        $resolved = $null
        foreach ($directory in $candidates) {
            $full = [IO.Path]::GetFullPath((Join-Path $directory $include))
            if (!$full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
            $relative = $full.Substring($root.Length + 1).Replace('\', '/')
            if (!$paths.ContainsKey($relative)) { continue }
            if ($relative -cne $paths[$relative]) { throw "Include casing mismatch in ${path}: $include" }
            $resolved = $relative
            break
        }
        if (!$resolved -and $include.StartsWith('Fallout4HDR/')) {
            throw "Missing public project include in ${path}: $include"
        }
        if ($path.StartsWith('include/') -and $resolved -like 'src/*') {
            throw "Public header depends on private implementation: $path"
        }
        $oracle = $path -ceq 'tests/unit/tonemap/Gt7Tests.cpp' -and
            $include -ceq '../../../third_party/gt7/upstream/gt7_tone_mapping.cpp'
        if ($include.EndsWith('.cpp') -and !$oracle) { throw "Implementation include in ${path}: $include" }
    }
}
Write-Host 'PASS maintained source names, include casing, and public/private header boundary'
