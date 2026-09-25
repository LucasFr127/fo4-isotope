# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param([switch]$SelfTest)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot 'scripts/internal/SourceText.ps1')
$includeRoots = @('', 'include', 'src', 'src/config', 'src/game', 'src/graphics', 'src/ui', 'src/runtime',
    'tests/unit', 'tests/support', 'tests/shader')

function Get-LayoutPath {
    param([string]$Path)
    return [IO.Path]::GetRelativePath($repoRoot, $Path).Replace('\', '/')
}

function Add-LayoutPath {
    param([hashtable]$Paths, [string]$Path)
    if ($Paths.ContainsKey($Path) -and $Paths[$Path] -cne $Path) {
        throw "Case-insensitive path collision: $Path and $($Paths[$Path])"
    }
    $Paths[$Path] = $Path
}

function Resolve-LayoutInclude {
    param([string]$File, [string]$Include, [hashtable]$Paths)
    $roots = @((Split-Path $File -Parent)) + $includeRoots
    foreach ($root in $roots) {
        $candidate = Get-LayoutPath ([IO.Path]::GetFullPath(
            (Join-Path (Join-Path $repoRoot $root) $Include)))
        if ($Paths.ContainsKey($candidate)) {
            if ($candidate -cne $Paths[$candidate]) {
                throw "Include casing in ${File}: $Include should resolve to $($Paths[$candidate])"
            }
            return $candidate
        }
    }
    return $null
}

function Assert-LayoutHeaderBoundary {
    param([string]$File, [string]$Include, [string]$Resolved)
    $privateInclude = $Resolved -like 'src/*' -or $Include -match '(^|[/\\])src[/\\]'
    if ($File.StartsWith('include/') -and $privateInclude) {
        throw "Shared header includes private source in ${File}: $Include"
    }
}

function Assert-LayoutCoreDependency {
    param([string]$File, [string]$Resolved)
    $core = $File -match '^(src|include/Fallout4HDR)/(config|tonemap|output|runtime|hooks)/'
    $renderer = $Resolved -match '^(src|include/Fallout4HDR)/(graphics|ui)/'
    $inputContract = $Resolved -ceq 'include/Fallout4HDR/ui/HdrSettingsInput.hpp'
    if (!$core -or !$renderer) { return }
    if (!$inputContract) {
        throw "Core component depends on renderer/UI in ${File}: $Resolved"
    }
}

function Assert-LayoutDependency {
    param([string]$File, [string]$Resolved)
    Assert-LayoutCoreDependency $File $Resolved
    if ($Resolved -notmatch '^src/(graphics|ui)/') { return }
    $owner = $Matches[1]
    $ownerSource = $File.StartsWith("src/$owner/")
    $scopedTest = $File -match '^tests/(unit|shader|support)/'
    if (!$ownerSource -and !$scopedTest) {
        throw "Private $owner component consumed outside its owner in ${File}: $Resolved"
    }
}

function Assert-LayoutPortablePath {
    param([string]$Path)
    foreach ($part in $Path.Split('/')) {
        if ($part -match '^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$)' -or
            $part -match '[<>:"\\|?*\x00-\x1f]|[. ]$') {
            throw "Nonportable path: $Path"
        }
    }
}

function Assert-LayoutDirectory {
    param([string]$Path)
    Assert-LayoutPortablePath $Path
    if ($Path -ceq 'include/Fallout4HDR' -or $Path -cmatch '^runtime/\d+\.\d+\.\d+$') { return }
    if ((Split-Path $Path -Leaf) -cnotmatch '^[a-z][a-z0-9_]*$') {
        throw "Subsystem directory must use lowercase words: $Path"
    }
}

function Assert-LayoutNativeName {
    param([string]$File, [hashtable]$Paths)
    Assert-LayoutPortablePath $File
    $name = Split-Path $File -Leaf
    if ($name -ceq 'main.cpp') { return }
    $pattern = '^[A-Z][A-Za-z0-9]*\.(hpp|cpp|inl|inc|hlsli)$'
    if ($File -match '\.hlsl$') { $pattern = '^[A-Z][A-Za-z0-9]*\.(vs|hs|ds|gs|ps|cs)\.hlsl$' }
    if ($name -cnotmatch $pattern) { throw "Invalid native filename: $File" }
    if ($File -match '\.inl$' -and
        !$Paths.ContainsKey([IO.Path]::ChangeExtension($File, '.hpp'))) {
        throw "Generic implementation fragment: $File"
    }
}

function Assert-LayoutPowerShellName {
    param([string]$File)
    Assert-LayoutPortablePath $File
    $name = Split-Path $File -Leaf
    if ($File -match '^scripts/[^/]+$' -and $name -cnotmatch '^[a-z][a-z0-9]*(-[a-z0-9]+)*\.ps1$') {
        throw "Command script must use lowercase hyphenated words: $File"
    }
    if ($File.StartsWith('tests/') -and $name -cnotmatch '^[A-Z][A-Za-z0-9]*\.Tests\.ps1$') {
        throw "PowerShell test must use Component.Tests.ps1 spelling: $File"
    }
}

function Assert-LayoutInlineInclude {
    param([string]$File, [string]$Include, [string]$Resolved)
    $unpairedHeader = $File -notmatch '\.hpp$' -or
        [IO.Path]::ChangeExtension($Resolved, '.hpp') -cne $File
    if ($Include -match '\.inl$' -and $unpairedHeader) {
        throw "Implementation fragment included in ${File}: $Include"
    }
}

function Assert-LayoutInclude {
    param([string]$File, [string]$Include, [hashtable]$Paths)
    # The pinned GT7 oracle supplies its class and sample main in one immutable source file.
    $pinnedGt7Oracle = $File -ceq 'tests/unit/tonemap/Gt7Tests.cpp' -and
        $Include -ceq '../../../third_party/gt7/upstream/gt7_tone_mapping.cpp'
    if ($Include -match '\.cpp$' -and !$pinnedGt7Oracle) {
        throw "Implementation included in ${File}: $Include"
    }
    $resolved = Resolve-LayoutInclude $File $Include $Paths
    Assert-LayoutHeaderBoundary $File $Include $resolved
    Assert-LayoutDependency $File $resolved
    Assert-LayoutInlineInclude $File $Include $resolved
    $missingSharedInclude = !$resolved -and $Include -match '^Fallout4HDR/' -and
        $Include -notmatch '\.generated\.hpp$'
    if ($missingSharedInclude) {
        throw "Missing shared include in ${File}: $Include"
    }
}

function Assert-LayoutFile {
    param([string]$File, [hashtable]$Paths)
    Assert-LayoutNativeName $File $Paths
    $text = Hide-Fo4HdrNonCodeText -Text (Get-Content -LiteralPath (
        Join-Path $repoRoot $File) -Raw) -Language Cpp -PreserveLiterals
    foreach ($match in [regex]::Matches($text, '(?m)^\s*#\s*include\s*["<]([^">]+)[">]')) {
        Assert-LayoutInclude $File $match.Groups[1].Value $Paths
    }
}

function Assert-LayoutRejects {
    param([scriptblock]$Action)
    try { & $Action } catch { return }
    throw 'Architecture check accepted an invalid layout.'
}

function Test-LayoutDependencies {
    $paths = @{}
    foreach ($path in @('include/Fallout4HDR/graphics/HdrRenderer.hpp',
        'include/Fallout4HDR/ui/SettingsWindow.hpp', 'include/Fallout4HDR/ui/HdrSettingsInput.hpp',
        'src/graphics/PresentFrame.hpp', 'src/ui/LutProfilesPanel.hpp',
        'src/game/GameStateObserver.hpp', 'src/runtime/RuntimeIdentityFiles.hpp',
        'src/config/SettingsPersistence.hpp')) {
        Add-LayoutPath $paths $path
    }
    foreach ($subsystem in @('config', 'tonemap', 'output', 'runtime', 'hooks')) {
        foreach ($root in @('src', 'include/Fallout4HDR')) {
            $file = "$root/$subsystem/Component.hpp"
            foreach ($dependency in @('Fallout4HDR/graphics/HdrRenderer.hpp',
                'Fallout4HDR/ui/SettingsWindow.hpp', 'graphics/PresentFrame.hpp', 'ui/LutProfilesPanel.hpp')) {
                Assert-LayoutRejects { Assert-LayoutInclude $file $dependency $paths }
            }
        }
    }
    Assert-LayoutInclude 'src/config/Settings.cpp' 'Fallout4HDR/ui/HdrSettingsInput.hpp' $paths
    Assert-LayoutInclude 'src/graphics/PresentFrame.cpp' 'Fallout4HDR/ui/SettingsWindow.hpp' $paths
    Assert-LayoutInclude 'src/plugin/PluginMain.cpp' '../game/GameStateObserver.hpp' $paths
    Assert-LayoutInclude 'src/plugin/PluginMain.cpp' '../runtime/RuntimeIdentityFiles.hpp' $paths
    Assert-LayoutInclude 'src/game/GameStateObserver.cpp' 'Fallout4HDR/graphics/HdrRenderer.hpp' $paths
    Assert-LayoutInclude 'tests/unit/runtime/RuntimeIdentityFilesTests.cpp' 'RuntimeIdentityFiles.hpp' $paths
    Assert-LayoutInclude 'tests/unit/config/SettingsTests.cpp' 'LutProfilesPanel.hpp' $paths
    Assert-LayoutInclude 'tests/unit/config/SettingsTests.cpp' 'SettingsPersistence.hpp' $paths
    Assert-LayoutRejects { Assert-LayoutInclude 'include/Fallout4HDR/config/Settings.hpp' 'SettingsPersistence.hpp' $paths }
    Assert-LayoutInclude 'tests/shader/PresentationTests.cpp' 'PresentFrame.hpp' $paths
    Assert-LayoutRejects { Assert-LayoutInclude 'src/graphics/PresentFrame.cpp' '../ui/LutProfilesPanel.hpp' $paths }
    Assert-LayoutRejects { Assert-LayoutInclude 'src/ui/SettingsWindow.cpp' '../graphics/PresentFrame.hpp' $paths }
    Assert-LayoutRejects { Assert-LayoutInclude 'tests/unit/runtime/RuntimeIdentityFilesTests.cpp' 'runtimeIdentityFiles.hpp' $paths }
}

function Test-LayoutNames {
    $paths = @{ 'src/config/Template.hpp' = 'src/config/Template.hpp' }
    foreach ($file in @('src/config/Settings.cpp', 'src/config/Template.inl',
        'src/graphics/d3d11/ScopedD3D11State.hpp', 'tests/unit/main.cpp',
        'runtime/1.10.163/PluginVersion.cpp', 'shaders/output/PresentPixels.cs.hlsl',
        'shaders/output/DisplayMath.hlsli')) {
        Assert-LayoutNativeName $file $paths
    }
    foreach ($file in @('src/config/settings.cpp', 'src/config/Hdr_Settings.hpp',
        'src/config/Settings.h', 'src/config/Orphan.inl', 'src/config/CON.hpp',
        'shaders/output/PresentPixels.hlsl', 'shaders/output/PresentPixels.CS.hlsl')) {
        Assert-LayoutRejects { Assert-LayoutNativeName $file $paths }
    }
    foreach ($directory in @('include/Fallout4HDR', 'src/graphics/d3d11', 'runtime/1.10.163')) {
        Assert-LayoutDirectory $directory
    }
    foreach ($directory in @('src/Graphics', 'src/render-passes', 'src/con', 'src/graphics.')) {
        Assert-LayoutRejects { Assert-LayoutDirectory $directory }
    }
}

function Test-LayoutChecks {
    $paths = @{}
    Add-LayoutPath $paths 'include/Fallout4HDR/config/Settings.hpp'
    Add-LayoutPath $paths 'src/config/Private.hpp'
    Add-LayoutPath $paths 'src/config/Template.hpp'
    Add-LayoutPath $paths 'src/config/Template.inl'
    Assert-LayoutInclude 'src/config/Settings.cpp' 'Fallout4HDR/config/Settings.hpp' $paths
    Assert-LayoutInclude 'src/config/Template.hpp' 'Template.inl' $paths
    Assert-LayoutInclude 'tests/unit/tonemap/Gt7Tests.cpp' '../../../third_party/gt7/upstream/gt7_tone_mapping.cpp' $paths
    Assert-LayoutRejects { Assert-LayoutInclude 'tests/unit/tonemap/Gt7Tests.cpp' '../../../third_party/gt7/upstream/other.cpp' $paths }
    Assert-LayoutRejects { Assert-LayoutInclude 'tests/unit/tonemap/OtherTests.cpp' '../../../third_party/gt7/upstream/gt7_tone_mapping.cpp' $paths }
    Assert-LayoutRejects { Add-LayoutPath $paths 'SRC/config/Private.hpp' }
    Assert-LayoutRejects { Assert-LayoutInclude 'src/config/Settings.cpp' 'fallout4hdr/config/Settings.hpp' $paths }
    Assert-LayoutRejects { Assert-LayoutInclude 'src/config/Settings.cpp' 'Settings.cpp' $paths }
    Assert-LayoutRejects { Assert-LayoutInclude 'src/config/Settings.cpp' 'Template.inl' $paths }
    Assert-LayoutRejects { Assert-LayoutInclude 'include/Fallout4HDR/config/Settings.hpp' '../../../src/config/Private.hpp' $paths }
    Test-LayoutDependencies
    Test-LayoutNames
    Test-LayoutPowerShellNames
    Write-Host 'Architecture layout check self-test passed.'
}

function Test-LayoutPowerShellNames {
    foreach ($file in @('scripts/build.ps1', 'scripts/compile-shaders.ps1',
        'scripts/audit-f4se-project.ps1', 'scripts/internal/Project.Runtime.Binary.ps1',
        'tests/runtime/AddressLibraryV0.Tests.ps1')) {
        Assert-LayoutPowerShellName $file
    }
    foreach ($file in @('scripts/Build.ps1', 'scripts/compile_shaders.ps1',
        'scripts/compile--shaders.ps1', 'tests/source/SourceText.tests.ps1',
        'tests/source/sourceText.Tests.ps1', 'tests/source/SourceText.ps1')) {
        Assert-LayoutRejects { Assert-LayoutPowerShellName $file }
    }
}

if ($SelfTest) { Test-LayoutChecks; return }
$paths = @{}
$files = @()
foreach ($root in @('include', 'src', 'tests', 'runtime', 'shaders', 'scripts')) {
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $repoRoot $root) -Recurse) {
        $path = Get-LayoutPath $item.FullName
        if ($path -match '(^|/)(generated|third_party|vendor|private)/' -or
            $item.Name -match '\.generated\.') { continue }
        if ($item.PSIsContainer) {
            Assert-LayoutDirectory $path
            Add-LayoutPath $paths $path
            continue
        }
        if ($item.Extension -eq '.ps1') {
            Assert-LayoutPowerShellName $path
            Add-LayoutPath $paths $path
            continue
        }
        if ($item.Extension -notin @('.hpp', '.h', '.cpp', '.inl', '.inc', '.hlsl', '.hlsli')) { continue }
        Add-LayoutPath $paths $path
        $files += $path
    }
}
foreach ($file in $files) { Assert-LayoutFile $file $paths }
Write-Host "Architecture layout checks passed for $($files.Count) owned native files."
