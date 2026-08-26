# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '../..'))
$prefix = $root + [IO.Path]::DirectorySeparatorChar

function Read-SelectedPaths {
    param([string]$Relative, [string]$Variable)

    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        [IO.Path]::Combine($root, $Relative), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Cannot parse source selection: $Relative" }
    $assignments = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.VariablePath.UserPath -ceq $Variable
    }, $true))
    if ($assignments.Count -ne 1) { throw "Ambiguous source selection: $Relative" }
    return @($assignments[0].Right.FindAll({ param($node)
        $node -is [Management.Automation.Language.StringConstantExpressionAst]
    }, $true) | ForEach-Object Value)
}

function Assert-SourceName {
    param([string]$Relative)

    $segments = $Relative.Split('/')
    if ([IO.Path]::IsPathRooted($Relative)) { throw "Rooted source path: $Relative" }
    if ($Relative.IndexOfAny([char[]]'\:') -ge 0) { throw "Invalid source separator: $Relative" }
    if (@($segments | Where-Object { $_ -in @('', '.', '..', '.git') }).Count) {
        throw "Unsafe source path: $Relative"
    }
}

function Assert-SelectedFile {
    param([string]$Relative)

    Assert-SourceName $Relative
    $path = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $Relative))
    if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source path leaves project: $Relative"
    }
    if (-not [IO.File]::Exists($path)) { throw "Missing selected source: $Relative" }
    if (([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Selected source is a reparse point: $Relative"
    }
}

function Assert-BuildInputs {
    param([string]$Recipe, [Collections.Generic.HashSet[string]]$Selected)

    # Dependency-relative literals are checked against their configured roots.
    $projectRecipe = [regex]::Replace($Recipe, 'path\.join\(dependency_profile\.[^)]*\)', '')
    foreach ($input in [regex]::Matches($projectRecipe,
        '"((?:src|tests|include|shaders)/[^"$]+\.(?:cpp|hpp|h|rc|hlsl|hlsli))"')) {
        if (-not $Selected.Contains($input.Groups[1].Value)) {
            throw "Build input is not selected: $($input.Groups[1].Value)"
        }
    }
}

function Assert-LocalIncludes {
    param([string]$Relative, [string[]]$IncludeRoots,
        [Collections.Generic.HashSet[string]]$Selected)

    $path = [IO.Path]::Combine($root, $Relative)
    $directory = [IO.Path]::GetDirectoryName($path)
    foreach ($include in [regex]::Matches([IO.File]::ReadAllText($path),
        '(?m)^\s*#\s*include\s*"([^"]+)"')) {
        $name = $include.Groups[1].Value
        if (-not (Find-SelectedInclude $name (@($directory) + $IncludeRoots) $Selected)) {
            throw "Unselected local include: $Relative -> $name"
        }
    }
}

function Find-SelectedInclude {
    param([string]$Name, [string[]]$SearchRoots,
        [Collections.Generic.HashSet[string]]$Selected)

    foreach ($search in $SearchRoots) {
        $candidate = [IO.Path]::GetFullPath([IO.Path]::Combine($search, $Name))
        if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $relative = [IO.Path]::GetRelativePath($root, $candidate).Replace('\', '/')
        if ($Selected.Contains($relative)) { return $true }
    }
    return $false
}

function Assert-GeneratedIncludes {
    param([string]$Relative, [string]$Recipe,
        [Collections.Generic.HashSet[string]]$Selected)

    $text = [IO.File]::ReadAllText([IO.Path]::Combine($root, $Relative))
    foreach ($include in [regex]::Matches($text, '<fo4hdr-shaders/([A-Za-z0-9]+)\.hpp>')) {
        $name = $include.Groups[1].Value
        $source = "shaders/fallout4/$name.ps.hlsl"
        $declared = $Recipe.Contains('"' + $name + '"')
        if ($name -in @('PresentPixels', 'PrepareBackdrop', 'PrepareHud', 'CopySdrCodes')) {
            $source = 'shaders/output/PresentPixels.cs.hlsl'
            $declared = $name -ceq 'PresentPixels' -or $declared
        }
        if ($name -ceq 'DeveloperExposureCapture') {
            $source = 'shaders/diagnostics/DeveloperExposureCapture.cs.hlsl'
            $declared = $Recipe.Contains('"kDeveloperExposureCaptureBytecode"')
        }
        if ($name -ceq 'ArmorEncodedComposition') {
            $source = 'shaders/armor/EncodedComposition.cs.hlsl'
            $declared = $Recipe.Contains('"kArmorEncodedCompositionBytecode"')
        }
        if (-not $declared -or -not $Selected.Contains($source)) {
            throw "Generated include lacks its selected shader recipe: $name"
        }
    }
}

function Get-DependencyRoots {
    $commonlib = [IO.Path]::Combine($root, 'lib/commonlibf4')
    return @{
        commonlib = $commonlib
        shared = [IO.Path]::Combine($commonlib, 'lib/commonlib-shared')
        spdlog = if ($env:FO4HDR_SPDLOG_SOURCE) { $env:FO4HDR_SPDLOG_SOURCE }
            else { [IO.Path]::Combine($root, 'lib/spdlog') }
        imgui = if ($env:FO4HDR_IMGUI_SOURCE) { $env:FO4HDR_IMGUI_SOURCE }
            else { [IO.Path]::Combine($root, 'lib/imgui') }
    }
}

function Assert-DependencyPath {
    param([string]$Path)

    if (-not $Path.Contains('*')) {
        if (-not (Test-Path -LiteralPath $Path)) { throw "Missing dependency build input: $Path" }
        return
    }
    if (-not @(Get-ChildItem -Path $Path -File -Recurse).Count) {
        throw "Missing dependency source pattern: $Path"
    }
}

function Assert-DependencyTerms {
    param([hashtable]$Roots)

    foreach ($entry in @(@('commonlib', 'LICENSE'), @('shared', 'LICENSE'),
        @('shared', 'EXCEPTIONS'), @('spdlog', 'LICENSE'), @('imgui', 'LICENSE.txt'))) {
        if (-not [IO.File]::Exists([IO.Path]::Combine($roots[$entry[0]], $entry[1]))) {
            throw "Missing dependency terms: $($entry[0])/$($entry[1])"
        }
    }
}

function Assert-DependencyInputs {
    param([string]$Recipe)

    $roots = Get-DependencyRoots
    foreach ($input in [regex]::Matches($Recipe,
        'path\.join\(dependency_profile\.(commonlib|shared|spdlog|imgui)_source, "([^"]+)"\)')) {
        Assert-DependencyPath ([IO.Path]::Combine(
            $roots[$input.Groups[1].Value], $input.Groups[2].Value))
    }
    Assert-DependencyTerms $roots
}

function Read-SourceSelection {
    $paths = @(Read-SelectedPaths 'scripts/package-source.ps1' 'files')
    $expected = @(Read-SelectedPaths 'tests/source/PackageSource.Tests.ps1' 'expected')
    $selected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $paths) {
        Assert-SelectedFile $path
        if (-not $selected.Add($path)) { throw "Duplicate selected source: $path" }
    }
    if (Compare-Object $paths $expected -CaseSensitive) { throw 'Source selections differ.' }
    return ,$selected
}

function Test-SourceClosure {
    $selected = Read-SourceSelection
    $paths = @($selected)
    $xmake = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'xmake.lua'))
    $shaders = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'scripts/compile-shaders.ps1'))
    Assert-BuildInputs $xmake $selected
    Assert-BuildInputs $shaders $selected
    $includeRoots = @([regex]::Matches($xmake, '(?m)^\s*add_includedirs\(([^\r\n]+)') |
        ForEach-Object { [regex]::Matches($_.Groups[1].Value, '"([^"$]+)"') } |
        ForEach-Object { [IO.Path]::Combine($root, $_.Groups[1].Value) })
    foreach ($path in $paths | Where-Object { $_ -match '\.(cpp|hpp|h|hlsl|hlsli)$' }) {
        Assert-LocalIncludes $path $includeRoots $selected
        Assert-GeneratedIncludes $path $shaders $selected
    }
    Assert-DependencyInputs $xmake
    $lock = Get-Content -LiteralPath ([IO.Path]::Combine($root, 'third_party/gt7/UPSTREAM.lock.json')) -Raw |
        ConvertFrom-Json
    $oracle = [IO.Path]::Combine($root, 'third_party/gt7/upstream/gt7_tone_mapping.cpp')
    if ((Get-FileHash -LiteralPath $oracle -Algorithm SHA256).Hash -ine $lock.sha256) {
        throw 'GT7 source differs from its retained identity.'
    }
    Write-Host "PASS selected source/build/include closure ($($paths.Count) project files); compilation and archive identity are separate checks."
}

Test-SourceClosure
