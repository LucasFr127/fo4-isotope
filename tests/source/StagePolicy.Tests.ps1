# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

param([ValidateSet('debug', 'releasedbg')][string]$Mode = 'releasedbg')

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '../..'))
$stage = [IO.Path]::Combine($root, 'dist/1.10.163/Data/F4SE/Plugins')
$build = [IO.Path]::Combine($root, "build/windows/x64/$Mode")

function Assert-StageDirectories {
    $directory = [IO.DirectoryInfo]::new($stage)
    while ($directory.FullName -ne $root) {
        if (-not $directory.Exists -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Missing or redirected staging directory: $($directory.FullName)"
        }
        $directory = $directory.Parent
    }
}

function Assert-StagedFile {
    param([string]$Name, [string]$Source)

    $file = [IO.FileInfo]::new([IO.Path]::Combine($stage, $Name))
    if (-not $file.Exists -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Missing or redirected staged file: $Name"
    }
    $actual = [IO.File]::ReadAllBytes($file.FullName)
    $expected = [IO.File]::ReadAllBytes($Source)
    if ([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($expected)) {
        throw "Staged bytes differ from source: $Name"
    }
}

Assert-StageDirectories
$required = @('Fallout4HDR.dll', 'Fallout4HDR.pdb', 'Fallout4HDR.ini')
$entries = @([IO.Directory]::EnumerateFileSystemEntries($stage))
if ($entries.Count -ne $required.Count) { throw 'Unexpected staging entry count.' }
foreach ($entry in $entries) {
    if ([IO.Path]::GetFileName($entry) -cnotin $required) { throw "Unexpected staging entry: $entry" }
}
Assert-StagedFile 'Fallout4HDR.dll' ([IO.Path]::Combine($build, 'Fallout4HDR.dll'))
Assert-StagedFile 'Fallout4HDR.pdb' ([IO.Path]::Combine($build, 'Fallout4HDR.pdb'))
Assert-StagedFile 'Fallout4HDR.ini' ([IO.Path]::Combine($root, 'config/Fallout4HDR.ini'))
Write-Host 'PASS staged plugin, symbols, canonical configuration, and exact file set'
