# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

function Assert-Throws {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    try {
        & $Action
    }
    catch {
        return
    }
    throw $Message
}

function Test-SelectedInventoryFiles {
    param([string]$TestRoot, [string]$Inventory)
    New-Item -ItemType Directory -Path (Join-Path $testRoot "src") |
        Out-Null
    [IO.File]::WriteAllText((Join-Path $testRoot "src\a.cpp"), "a")
    [IO.File]::WriteAllText((Join-Path $testRoot "src\B.cpp"), "b")

    [IO.File]::WriteAllLines($inventory, @(
        "# Inventory",
        "",
        '- `src/a.cpp`',
        '- `selection.md`',
        '- `src/B.cpp`'
    ))
    $actual = @(Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory)
    $expected = @("selection.md", "src/B.cpp", "src/a.cpp")
    if ([string]::Join("|", $actual) -cne [string]::Join("|", $expected)) {
        throw "Source-tree inventory did not return its ordinal-sorted paths."
    }

    foreach ($invalid in @(
        '- `../outside.txt`',
        '- `C:/outside.txt`',
        '- `src\a.cpp`',
        '- `.git/config`',
        '- `src/missing.cpp`'
    )) {
        [IO.File]::WriteAllLines($inventory, @('- `selection.md`', $invalid))
        Assert-Throws {
            Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory | Out-Null
        } "Source-tree inventory accepted: $invalid"
    }

    [IO.File]::WriteAllLines($inventory, @(
        '- `selection.md`',
        '- `src/a.cpp`',
        '- `SRC/A.CPP`'
    ))
    Assert-Throws {
        Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory | Out-Null
    } "Source-tree inventory accepted a case-insensitive duplicate."

}

function Test-InventoryDependencyFiles {
    param([string]$TestRoot, [string]$Inventory)
    $dependency = Join-Path $testRoot 'lib\dependency'
    New-Item -ItemType Directory -Path (Join-Path $dependency 'nested') | Out-Null
    [IO.File]::WriteAllText((Join-Path $dependency 'LICENSE'), "license`n")
    [IO.File]::WriteAllText((Join-Path $dependency 'nested\source.cpp'), 'source')
    [IO.File]::WriteAllText((Join-Path $testRoot '.gitmodules'),
        "[submodule `"dependency`"]`n`tpath = lib/dependency`n`turl = https://example.invalid/dependency`n")
    [IO.File]::WriteAllLines($inventory, @(
        '- `selection.md`',
        '- `lib/dependency` (submodule)'
    ))
    $expanded = @(Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory)
    $missingDependencyFiles = 'lib/dependency/LICENSE' -cnotin $expanded -or
        'lib/dependency/nested/source.cpp' -cnotin $expanded
    if ($expanded.Count -ne 3 -or $missingDependencyFiles) {
        throw 'Extracted source did not expand the dependency and its nested source.'
    }
    [IO.File]::WriteAllText((Join-Path $dependency 'nested\.git'), 'private Git pointer')
    Assert-Throws {
        Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory | Out-Null
    } 'Private dependency Git metadata was accepted.'
    Remove-Item -LiteralPath (Join-Path $dependency 'nested\.git')
    [IO.File]::AppendAllText($inventory, "- ``lib/dependency/LICENSE```n")
    Assert-Throws {
        Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory | Out-Null
    } 'An ordinary entry duplicated an expanded dependency file.'
    [IO.File]::WriteAllLines($inventory, @(
        '- `selection.md`',
        '- `lib/dependency` (submodule)'
    ))
    [IO.File]::WriteAllText((Join-Path $testRoot '.git'), 'checkout marker')
    Assert-Throws {
        Get-Fo4HdrSourceTreeFiles -Root $testRoot -InventoryPath $inventory | Out-Null
    } 'An uninitialized checkout dependency was treated as an extracted archive.'
}

function Test-PublicSourceSelection {
    param([string]$PublicRoot)
    New-Item -ItemType Directory -Path $publicRoot | Out-Null
    [IO.File]::WriteAllText((Join-Path $publicRoot 'source.cpp'), 'source')
    foreach ($directory in @('build', 'dist', '.xmake', 'third_party/xmake-package-sources')) {
        $output = Join-Path $publicRoot $directory
        New-Item -ItemType Directory -Path $output -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $output 'generated.txt'), 'output')
    }
    [IO.File]::WriteAllText((Join-Path $publicRoot 'local.env.ps1'), '# configuration')
    $actual = @(Get-Fo4HdrSourceTreeFiles -Root $publicRoot)
    if ($actual.Count -ne 1 -or $actual[0] -cne 'source.cpp') {
        throw 'Extracted source selection includes generated output or requires an inventory.'
    }
    Test-PublicGitSelection $publicRoot
}

function Test-PublicGitSelection {
    param([string]$PublicRoot)
    git init --quiet $publicRoot
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize source selection fixture.' }
    git -C $publicRoot add -- source.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Cannot stage source selection fixture.' }
    [IO.File]::WriteAllText((Join-Path $publicRoot 'untracked.cpp'), 'untracked')
    $actual = @(Get-Fo4HdrSourceTreeFiles -Root $publicRoot)
    if ($actual.Count -ne 1 -or $actual[0] -cne 'source.cpp') {
        throw 'Public checkout selection differs from its tracked source files.'
    }
    Test-ConfiguredSourceSelection $publicRoot
}

function Test-ConfiguredSourceSelection {
    param([string]$PublicRoot)
    [IO.File]::WriteAllText((Join-Path $publicRoot 'selection.md'), '- `untracked.cpp`')
    git -C $publicRoot config --local fo4hdr.sourceInventory selection.md
    if ($LASTEXITCODE -ne 0) { throw 'Cannot configure source selection fixture.' }
    $actual = @(Get-Fo4HdrSourceTreeFiles -Root $publicRoot)
    if ($actual.Count -ne 1 -or $actual[0] -cne 'untracked.cpp') {
        throw 'Checkout-local source selection did not override tracked files.'
    }
    git -C $publicRoot config --local fo4hdr.sourceInventory missing.md
    if ($LASTEXITCODE -ne 0) { throw 'Cannot configure source selection fixture.' }
    Assert-Throws {
        Get-Fo4HdrSourceTreeFiles -Root $publicRoot | Out-Null
    } 'A missing configured inventory silently fell back to other files.'
}

$testRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    ("FO4Isotope-source-tree-" + [Guid]::NewGuid().ToString("N"))
$inventory = Join-Path $testRoot "selection.md"

try {
    Test-SelectedInventoryFiles $testRoot $inventory
    Test-InventoryDependencyFiles $testRoot $inventory
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

$publicRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'FO4Isotope-public-source-' + [Guid]::NewGuid().ToString('N'))
try {
    Test-PublicSourceSelection $publicRoot
}
finally {
    if (Test-Path -LiteralPath $publicRoot) {
        Remove-Item -LiteralPath $publicRoot -Recurse -Force
    }
}

$sourceFiles = [Collections.Generic.HashSet[string]]::new(
    [string[]]@(Get-Fo4HdrSourceTreeFiles -Root $repoRoot),
    [StringComparer]::Ordinal
)
foreach ($relative in $sourceFiles) {
    if ($relative.StartsWith('lib/', [StringComparison]::Ordinal) -or
        [IO.Path]::GetExtension($relative) -ne '.md') {
        continue
    }
    $path = Join-Path $repoRoot $relative
    $text = Get-Content -LiteralPath $path -Raw
    foreach ($link in [regex]::Matches($text, '\[[^\]\r\n]*\]\(([^)\r\n]+)\)')) {
        $target = $link.Groups[1].Value.Trim('<', '>')
        if ($target -match '^(?:[a-z][a-z0-9+.-]*:|#)') {
            continue
        }
        $target = [Uri]::UnescapeDataString(($target -split '[?#]', 2)[0])
        $resolved = [IO.Path]::GetFullPath((Join-Path (Split-Path $path) $target))
        $linkedFile = [IO.Path]::GetRelativePath($repoRoot, $resolved).Replace('\', '/')
        if (-not $sourceFiles.Contains($linkedFile)) {
            throw "Public documentation links outside the source selection: $relative -> $target"
        }
    }
}

Write-Host "Source-tree inventory and public documentation link regression tests passed."
