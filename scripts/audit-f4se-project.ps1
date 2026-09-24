# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Root,

    [switch]$Json,

    [ValidateSet("none", "low", "medium", "high", "critical")]
    [string]$FailOn = "none",

    [string[]]$ReRoot = @(),

    [string[]]$AllowReFile = @(),

    [string[]]$F4seFile = @(),

    [string[]]$PluginPchFile = @(),

    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/SourceText.ps1")
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")
Set-StrictMode -Version Latest
$F4seFile = @($F4seFile | ForEach-Object { $_ -split ',' } | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
})

$script:SeverityRank = [ordered]@{
    none = 0
    low = 1
    medium = 2
    high = 3
    critical = 4
}
$script:SourceSuffixes = @(
    ".c", ".cc", ".cpp", ".cppm", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ixx", ".mpp"
)

function Add-Fo4HdrAddressFindings {
    param([string]$Code, [string]$Raw, $Findings, [string]$RelativePath)
    $absolutePatterns = @(
        'reinterpret_cast\s*<[^>]+>\s*\(\s*(0x[0-9A-Fa-f]{8,})\s*\)',
        'REL::Relocation\s*<[^>]+>[^\n;=]*[({]\s*(0x[0-9A-Fa-f]{8,})',
        '(?:REL::safe_write|safe_write|WriteSafe|write_(?:call|branch|vfunc|nop|fill)|Detour(?:Attach|Function|Jump|Call)|VirtualProtect)\s*\(\s*(?:reinterpret_cast\s*<[^>]+>\s*\(\s*)?(0x[0-9A-Fa-f]{8,})'
    )
    foreach ($pattern in $absolutePatterns) {
        foreach ($match in [regex]::Matches($Code, $pattern)) {
            $value = $match.Groups[1].Value
            if (-not (Test-Fo4HdrProcessAddress -Value $value)) { continue }
            $line = Get-Fo4HdrLineNumber -Text $Code -Offset $match.Index
            Add-Fo4HdrFinding -Findings $Findings -Finding (New-Fo4HdrFinding -Rule ([pscustomobject]@{
                    Code = 'F4SE001'
                    Severity = 'high'
                    Title = "Hard-coded process address"
                    Remediation = "Resolve the site through the pinned CommonLibF4 relocation mechanism, record provenance for every supported runtime, validate the target, and gate unsupported runtimes."
                    Confidence = 'high'
                }) `
                -Path $relativePath -Line $line -Evidence (Get-Fo4HdrLineText $Raw $line))
        }
    }

}

function Write-Fo4HdrAuditHeader {
    param([psobject]$Summary)
    Write-Output "CommonLibF4/F4SE heuristic audit"
    Write-Output "Root: $($Summary.root)"
    Write-Output "Files scanned: $($Summary.files_scanned)"
    if ($Summary.re_boundary_roots.Count -or $Summary.re_boundary_files.Count) {
        $roots = if ($Summary.re_boundary_roots.Count) { [string]::Join(', ', $Summary.re_boundary_roots) } else { '<none>' }
        $files = if ($Summary.re_boundary_files.Count) { [string]::Join(', ', $Summary.re_boundary_files) } else { '<none>' }
        Write-Output "RE boundary: roots=$roots; files=$files"
    }
    if ($Summary.f4se_boundary_files.Count) {
        Write-Output "F4SE/REL boundary files: $([string]::Join(', ', $Summary.f4se_boundary_files))"
    }
    if ($Summary.plugin_pch_files.Count) {
        Write-Output "Declared plugin PCH files: $([string]::Join(', ', $Summary.plugin_pch_files))"
    }
    Write-Output ""
}

function Test-Fo4HdrGameBoundary {
    param([IO.FileInfo]$File, [hashtable]$Boundary)
    if ($File.FullName -in $Boundary.ReFiles) { return $true }
    foreach ($root in $Boundary.ReRoots) {
        if (Test-Fo4HdrPathWithin $File.FullName $root) { return $true }
    }
    return $false
}

function Test-Fo4HdrAllowedEngineUse {
    param([IO.FileInfo]$File, [string]$Code, $ReBoundaryUse, [hashtable]$Boundary)
    if (Test-Fo4HdrGameBoundary $File $Boundary) { return $true }
    if ($null -eq $ReBoundaryUse -and $File.FullName -in $Boundary.F4seFiles) { return $true }
    if ($File.FullName -in $Boundary.PluginPchFiles) { return Test-Fo4HdrIncludeOnlyPluginPch $Code }
    return $false
}

function Test-Fo4HdrSameFinding {
    param([psobject]$Left, [psobject]$Right)
    return $Left.code -ceq $Right.code -and $Left.path -ceq $Right.path -and
        $Left.line -eq $Right.line -and $Left.title -ceq $Right.title
}
function Get-Fo4HdrCanonicalPath {
    param([Parameter(Mandatory)][string]$Path)

    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Get-Fo4HdrRelativePath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ProjectRoot
    )

    return [IO.Path]::GetRelativePath($ProjectRoot, $Path).Replace("\", "/")
}

function Resolve-Fo4HdrRepositoryPath {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$Value
    )

    $candidate = if ([IO.Path]::IsPathFullyQualified($Value)) { $Value } else { Join-Path $ProjectRoot $Value }
    $resolved = Get-Fo4HdrCanonicalPath -Path $candidate
    if (-not (Test-Fo4HdrPathWithin $resolved $ProjectRoot)) {
        throw "path escapes project root: $Value"
    }
    return $resolved
}

function Test-Fo4HdrPathWithin {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Directory
    )

    $relative = [IO.Path]::GetRelativePath($Directory, $Path)
    return $relative -ne ".." -and
        -not $relative.StartsWith("..$([IO.Path]::DirectorySeparatorChar)", [StringComparison]::Ordinal) -and
        -not [IO.Path]::IsPathFullyQualified($relative)
}

function Get-Fo4HdrCandidateFiles {
    param([Parameter(Mandatory)][string]$ProjectRoot)

    return @(
        Get-Fo4HdrSourceTreeFiles -Root $ProjectRoot | Where-Object {
            $_ -notmatch '^(lib|third_party)/' -and
                ([IO.Path]::GetExtension($_).ToLowerInvariant() -in $script:SourceSuffixes -or
                    $_ -ceq 'xmake.lua')
        } | ForEach-Object {
            Get-Item -LiteralPath (Join-Path $ProjectRoot $_)
        } | Sort-Object { (Get-Fo4HdrRelativePath -Path $_.FullName -ProjectRoot $ProjectRoot).ToLowerInvariant() }
    )
}

function Read-Fo4HdrAuditText {
    param([Parameter(Mandatory)][string]$Path)

    try {
        return [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $true))
    } catch [Text.DecoderFallbackException] {
        return [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $false))
    }
}

function Get-Fo4HdrLineNumber {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory)][int]$Offset
    )

    if ($Offset -le 0) { return 1 }
    $count = 1
    for ($index = 0; $index -lt [Math]::Min($Offset, $Text.Length); ++$index) {
        if ($Text[$index] -eq "`n") { ++$count }
    }
    return $count
}

function Get-Fo4HdrLineText {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory)][int]$Line
    )

    $lines = $Text -split "\r?\n"
    if ($Line -lt 1 -or $Line -gt $lines.Count) { return "" }
    $normalized = ($lines[$Line - 1].Trim() -replace "\s+", " ")
    return $normalized.Substring(0, [Math]::Min(240, $normalized.Length))
}

function New-Fo4HdrFinding {
    param(
        [Parameter(Mandatory)][psobject]$Rule,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][int]$Line,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Evidence
    )
    return [pscustomobject][ordered]@{
                    code = $Rule.Code
                    severity = $Rule.Severity
                    path = $Path
                    line = $Line
                    title = $Rule.Title
                    evidence = $Evidence
                    remediation = $Rule.Remediation
                    confidence = $Rule.Confidence
                }
}
function Add-Fo4HdrFinding {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][Collections.Generic.List[object]]$Findings,
        [Parameter(Mandatory)][psobject]$Finding
    )

    foreach ($existing in $Findings) {
        if ((Test-Fo4HdrSameFinding $existing $Finding)) {
            return
        }
    }
    $Findings.Add($Finding)
}

function Test-Fo4HdrProcessAddress {
    param([Parameter(Mandatory)][string]$Value)

    $parsed = 0L
    return [Int64]::TryParse($Value.Substring(2), [Globalization.NumberStyles]::HexNumber,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed) -and $parsed -ge 0x10000000
}

function Get-Fo4HdrEngineSemanticPattern {
    param([Parameter(Mandatory)][string]$NamespacePattern)

    $qualifiedUse = '(?<!\w)(?:::\s*)?(?:{0})\b\s*::' -f $NamespacePattern
    $usingDirective = '\busing\s+namespace\s+(?:::\s*)?(?:{0})\b' -f $NamespacePattern
    $namespaceAlias = '\bnamespace\s+[A-Za-z_]\w*\s*=\s*(?:::\s*)?(?:{0})\b' -f $NamespacePattern
    return "(?:$qualifiedUse)|(?:$usingDirective)|(?:$namespaceAlias)"
}

function Find-Fo4HdrEngineBoundaryUse {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Raw,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Code,
        [Parameter(Mandatory)][string]$NamespacePattern
    )

    $includeMatches = @([regex]::Matches(
        $Raw,
        ('^\s*#\s*include\s*[<"](?:{0})/' -f $NamespacePattern),
        [Text.RegularExpressions.RegexOptions]::Multiline) | Where-Object {
        $lineStart = $_.Index
        $lineEnd = $Code.IndexOf("`n", $lineStart)
        if ($lineEnd -lt 0) { $lineEnd = $Code.Length }
        [regex]::IsMatch(
            $Code.Substring($lineStart, $lineEnd - $lineStart),
            '^\s*#\s*include\b')
    })
    $semanticPattern = Get-Fo4HdrEngineSemanticPattern -NamespacePattern $NamespacePattern
    $matches = @(
        [regex]::Match($Code, $semanticPattern)
        $includeMatches
    ) | Where-Object Success | Sort-Object Index
    return $matches | Select-Object -First 1
}

function Test-Fo4HdrIncludeOnlyPluginPch {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Code
    )

    $semanticPattern = Get-Fo4HdrEngineSemanticPattern -NamespacePattern 'RE|F4SE|REL'
    if ([regex]::IsMatch(
        $Code,
        "^\s*#(?!\s*(?:include|pragma)\b).*(?:$semanticPattern)",
        [Text.RegularExpressions.RegexOptions]::Multiline)) {
        return $false
    }

    $withoutDirectives = [regex]::Replace(
        $Code,
        '^\s*#\s*(?:include|pragma)\b.*$',
        '',
        [Text.RegularExpressions.RegexOptions]::Multiline)
    return -not [regex]::IsMatch($withoutDirectives, $semanticPattern)
}

function Get-Fo4HdrProjectSummary {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][object[]]$Files,
        [Parameter(Mandatory)][hashtable]$Boundary
    )

    return [pscustomobject][ordered]@{
        root = $ProjectRoot.Replace("\", "/")
        files_scanned = $Files.Count
        re_boundary_roots = @($Boundary.ReRoots | ForEach-Object { Get-Fo4HdrRelativePath -Path $_ -ProjectRoot $ProjectRoot })
        re_boundary_files = @($Boundary.ReFiles | ForEach-Object { Get-Fo4HdrRelativePath -Path $_ -ProjectRoot $ProjectRoot })
        f4se_boundary_files = @($Boundary.F4seFiles | ForEach-Object { Get-Fo4HdrRelativePath -Path $_ -ProjectRoot $ProjectRoot })
        plugin_pch_files = @($Boundary.PluginPchFiles | ForEach-Object { Get-Fo4HdrRelativePath -Path $_ -ProjectRoot $ProjectRoot })
    }
}

function Invoke-Fo4HdrFileAudit {
    param(
        [Parameter(Mandatory)][IO.FileInfo]$File,
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Raw,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Code,
        [Parameter(Mandatory)][AllowEmptyCollection()][Collections.Generic.List[object]]$Findings
    )

    $relativePath = Get-Fo4HdrRelativePath -Path $File.FullName -ProjectRoot $ProjectRoot
    Add-Fo4HdrAddressFindings $Code $Raw $Findings $relativePath

    $engineIntegrated = [regex]::IsMatch($Code, '\b(?:RE|F4SE|REL)::') -or
        [regex]::IsMatch($Raw, '^\s*#\s*include\s*[<"](?:RE|F4SE|REL)/', [Text.RegularExpressions.RegexOptions]::Multiline)
    if ($engineIntegrated) {
        foreach ($match in [regex]::Matches($Code, '\b(?:std::(?:thread|jthread|async)|CreateThread|_beginthreadex)\b')) {
            $line = Get-Fo4HdrLineNumber -Text $Code -Offset $match.Index
            Add-Fo4HdrFinding -Findings $Findings -Finding (New-Fo4HdrFinding -Rule ([pscustomobject]@{
                    Code = 'F4SE004'
                    Severity = 'medium'
                    Title = "Worker thread in engine-integrated code"
                    Remediation = "Document ownership and shutdown, keep game-state access off the worker, and marshal engine mutation through the project's verified F4SE task/main-thread mechanism."
                    Confidence = 'medium'
                }) `
                -Path $relativePath -Line $line -Evidence (Get-Fo4HdrLineText $Raw $line))
        }
        foreach ($match in [regex]::Matches($Code, '\.\s*detach\s*\(\s*\)')) {
            $line = Get-Fo4HdrLineNumber -Text $Code -Offset $match.Index
            Add-Fo4HdrFinding -Findings $Findings -Finding (New-Fo4HdrFinding -Rule ([pscustomobject]@{
                    Code = 'F4SE008'
                    Severity = 'high'
                    Title = "Detached thread in an injected plugin"
                    Remediation = "Give the worker an owned lifetime and a bounded shutdown protocol; do not let it outlive captured plugin state or call game APIs from an unowned thread."
                    Confidence = 'high'
                }) `
                -Path $relativePath -Line $line -Evidence (Get-Fo4HdrLineText $Raw $line))
        }
    }

    foreach ($match in [regex]::Matches($Code, 'catch\s*\(\s*\.\.\.\s*\)\s*\{\s*\}', [Text.RegularExpressions.RegexOptions]::Singleline)) {
        $line = Get-Fo4HdrLineNumber -Text $Code -Offset $match.Index
        Add-Fo4HdrFinding -Findings $Findings -Finding (New-Fo4HdrFinding -Rule ([pscustomobject]@{
                    Code = 'F4SE005'
                    Severity = 'medium'
                    Title = "Empty catch-all suppresses failure"
                    Remediation = "At the ABI boundary, log actionable context and return/disable safely. Inside the plugin, catch only failures the code can handle without leaving partial state."
                    Confidence = 'high'
                }) `
                -Path $relativePath -Line $line -Evidence (Get-Fo4HdrLineText $Raw $line))
    }
    foreach ($match in [regex]::Matches($Code, '\bstd::(?:unique_ptr|shared_ptr)\s*<\s*RE::[A-Za-z_]\w*')) {
        $line = Get-Fo4HdrLineNumber -Text $Code -Offset $match.Index
        Add-Fo4HdrFinding -Findings $Findings -Finding (New-Fo4HdrFinding -Rule ([pscustomobject]@{
                    Code = 'F4SE009'
                    Severity = 'high'
                    Title = "Owning smart pointer appears to target a game-managed RE object"
                    Remediation = "Verify explicit ownership transfer and the required deleter. Otherwise keep the game object non-owning through the project/CommonLibF4 handle or pointer convention."
                    Confidence = 'medium'
                }) `
                -Path $relativePath -Line $line -Evidence (Get-Fo4HdrLineText $Raw $line))
    }
}

function Add-Fo4HdrBoundaryFindings {
    param([IO.FileInfo]$File, [string]$Raw, [string]$Code, [hashtable]$Boundary)
    $reBoundaryUse = Find-Fo4HdrEngineBoundaryUse `
        -Raw $raw `
        -Code $code `
        -NamespacePattern 'RE'
    $f4seBoundaryUse = Find-Fo4HdrEngineBoundaryUse `
        -Raw $raw `
        -Code $code `
        -NamespacePattern 'F4SE|REL'
    $boundaryCount = $Boundary.ReRoots.Count + $Boundary.ReFiles.Count +
        $Boundary.F4seFiles.Count + $Boundary.PluginPchFiles.Count
    if ($boundaryCount -eq 0) { return }
    if ($null -eq $reBoundaryUse -and $null -eq $f4seBoundaryUse) { return }
    if (Test-Fo4HdrAllowedEngineUse $file $code $reBoundaryUse $Boundary) { return }
    $boundaryUse = if ($null -ne $reBoundaryUse) {
        $reBoundaryUse
    } else {
        $f4seBoundaryUse
    }
    $line = Get-Fo4HdrLineNumber -Text $raw -Offset $boundaryUse.Index
    $rootsText = if ($Boundary.ReRoots.Count) { [string]::Join(", ", @($Boundary.ReRoots | ForEach-Object { Get-Fo4HdrRelativePath $_ $Boundary.Root })) } else { "<none>" }
    $filesText = if ($Boundary.ReFiles.Count) { [string]::Join(", ", @($Boundary.ReFiles | ForEach-Object { Get-Fo4HdrRelativePath $_ $Boundary.Root })) } else { "<none>" }
    Add-Fo4HdrFinding -Findings $Boundary.Findings -Finding (New-Fo4HdrFinding -Rule ([pscustomobject]@{
            Code = 'F4SE010'
            Severity = 'high'
            Title = "Engine type/header outside the declared boundary"
            Remediation = "Move semantic RE-dependent code behind the declared game adapter boundary. Keep F4SE/REL types out of plain public APIs; declare only the exact thin F4SE bootstrap file separately. An include-only plugin PCH may be declared separately, but its build-target scope still requires review. Allowed game roots: $rootsText; allowed game files: $filesText."
            Confidence = 'high'
        }) `
    -Path (Get-Fo4HdrRelativePath $file.FullName $Boundary.Root) -Line $line -Evidence (Get-Fo4HdrLineText $raw $line))
}

function Invoke-Fo4HdrProjectAudit {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [string[]]$AllowedReRoots = @(),
        [string[]]$AllowedReFiles = @(),
        [string[]]$AllowedF4seFiles = @(),
        [string[]]$PluginPchFiles = @()
    )

    $canonicalRoot = Get-Fo4HdrCanonicalPath -Path $ProjectRoot
    $resolvedRoots = @($AllowedReRoots | ForEach-Object { Resolve-Fo4HdrRepositoryPath -ProjectRoot $canonicalRoot -Value $_ })
    $resolvedFiles = @($AllowedReFiles | ForEach-Object { Resolve-Fo4HdrRepositoryPath -ProjectRoot $canonicalRoot -Value $_ })
    $resolvedF4seFiles = @($AllowedF4seFiles | ForEach-Object { Resolve-Fo4HdrRepositoryPath -ProjectRoot $canonicalRoot -Value $_ })
    $resolvedPluginPchFiles = @($PluginPchFiles | ForEach-Object {
        $resolved = Resolve-Fo4HdrRepositoryPath -ProjectRoot $canonicalRoot -Value $_
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "declared plugin PCH does not exist: $_"
        }
        $extension = [IO.Path]::GetExtension($resolved).ToLowerInvariant()
        if ($extension -notin @('.h', '.hh', '.hpp', '.hxx')) {
            throw "declared plugin PCH is not a header: $_"
        }
        $resolved
    })
    $files = @(Get-Fo4HdrCandidateFiles -ProjectRoot $canonicalRoot)
    $findings = [Collections.Generic.List[object]]::new()
    $boundary = @{
        Root = $canonicalRoot; Findings = $findings
        ReRoots = $resolvedRoots; ReFiles = $resolvedFiles
        F4seFiles = $resolvedF4seFiles; PluginPchFiles = $resolvedPluginPchFiles
    }
    $summary = Get-Fo4HdrProjectSummary $canonicalRoot $files $boundary
    foreach ($file in $files) {
        if ($file.Extension.ToLowerInvariant() -notin $script:SourceSuffixes) { continue }
        $raw = Read-Fo4HdrAuditText -Path $file.FullName
        $code = Hide-Fo4HdrNonCodeText -Text $raw
        Add-Fo4HdrBoundaryFindings $file $raw $code $boundary
        Invoke-Fo4HdrFileAudit -File $file -ProjectRoot $canonicalRoot -Raw $raw -Code $code -Findings $findings
    }

    $sorted = @($findings | Sort-Object `
        @{ Expression = { -$script:SeverityRank[$_.severity] } },
        @{ Expression = { $_.path.ToLowerInvariant() } },
        @{ Expression = { $_.line } },
        @{ Expression = { $_.code } })
    return [pscustomobject]@{ summary = $summary; findings = $sorted }
}

function Write-Fo4HdrAuditText {
    param(
        [Parameter(Mandatory)][psobject]$Summary,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Findings
    )

    Write-Fo4HdrAuditHeader $Summary
    if (-not $Findings.Count) {
        Write-Output "No heuristic findings. This does not validate runtime addresses, hook ABI, lifecycle timing, or in-game behavior."
        return
    }
    $counts = @("critical", "high", "medium", "low") | ForEach-Object {
        $severity = $_
        "$severity=$(@($Findings | Where-Object severity -eq $severity).Count)"
    }
    Write-Output "Findings: $([string]::Join(', ', $counts))"
    Write-Output ""
    foreach ($finding in $Findings) {
        Write-Output "[$($finding.severity.ToUpperInvariant())] $($finding.code) $($finding.title)"
        Write-Output "  Location: $($finding.path):$($finding.line)"
        if ($finding.evidence) { Write-Output "  Evidence: $($finding.evidence)" }
        Write-Output "  Fix: $($finding.remediation)"
        Write-Output "  Confidence: $($finding.confidence)"
        Write-Output ""
    }
}

function Test-UnsafeAuditFixture {
    param([string]$UnsafeRoot)
        New-Item -ItemType Directory -Path (Join-Path $unsafeRoot "src") -Force | Out-Null
        $unsafeSource = @'
#include "RE/Fallout.h"
#include <F4SE/F4SE.h>
#include <REL/Relocation.h>
#include <memory>
#include <thread>

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface*) {
    auto ptr = reinterpret_cast<void*>(0x140123456);
    std::thread worker([] {});
    worker.detach();
    std::unique_ptr<RE::TESForm> owned;
    try { REL::safe_write(reinterpret_cast<std::uintptr_t>(ptr), 0x90); } catch (...) {}
    return true;
}
'@
        [IO.File]::WriteAllText((Join-Path $unsafeRoot "src\main.cpp"), $unsafeSource, [Text.UTF8Encoding]::new($false))
        git init --quiet $unsafeRoot
        if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize audit fixture.' }
        git -C $unsafeRoot config --local fo4hdr.sourceInventory selection.md
        if ($LASTEXITCODE -ne 0) { throw 'Cannot configure audit fixture.' }
        [IO.File]::WriteAllLines((Join-Path $unsafeRoot "selection.md"), @(
            '- `selection.md`', '- `src/main.cpp`'
        ))
        $unsafe = Invoke-Fo4HdrProjectAudit -ProjectRoot $unsafeRoot -AllowedReRoots @("src/game")
        $expected = @("F4SE001", "F4SE004", "F4SE005", "F4SE008", "F4SE009", "F4SE010")
        $actual = @($unsafe.findings.code | Sort-Object -Unique)
        $missing = @($expected | Where-Object { $_ -notin $actual })
        $unexpected = @($actual | Where-Object { $_ -notin $expected })
        $codesDiffer = $missing.Count -or $unexpected.Count
        if ($unsafe.summary.files_scanned -ne 1 -or $codesDiffer) {
            throw (
                "unsafe fixture failed; " +
                "missing=$([string]::Join(',', $missing)); " +
                "unexpected=$([string]::Join(',', $unexpected)); " +
                "files_scanned=$($unsafe.summary.files_scanned)"
            )
        }

}

function Test-SafeAuditFixture {
    param([string]$SafeRoot)
        New-Item -ItemType Directory -Path (Join-Path $safeRoot "src") -Force | Out-Null
        $safeSource = @'
#include <RE/Fallout.h>
#include <F4SE/F4SE.h>
#include <REL/Relocation.h>
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface*) { return true; }
'@
        [IO.File]::WriteAllText((Join-Path $safeRoot "src\main.cpp"), $safeSource, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "unlisted.cpp"),
            "auto unlistedAddress = reinterpret_cast<void*>(0x140123456);`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "src\pch.h"),
            "#pragma once`n#include <RE/Fallout.h>`n#include <F4SE/F4SE.h>`n",
            [Text.UTF8Encoding]::new($false))
        git init --quiet $safeRoot
        if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize audit fixture.' }
        git -C $safeRoot config --local fo4hdr.sourceInventory selection.md
        if ($LASTEXITCODE -ne 0) { throw 'Cannot configure audit fixture.' }
        [IO.File]::WriteAllLines((Join-Path $safeRoot "selection.md"), @(
            '- `selection.md`', '- `src/main.cpp`', '- `src/pch.h`'
        ))
        $pchRejected = Invoke-Fo4HdrProjectAudit `
            -ProjectRoot $safeRoot `
            -AllowedReRoots @("src/game") `
            -AllowedReFiles @("src/main.cpp")
        if (-not ($pchRejected.findings | Where-Object {
            $_.code -eq 'F4SE010' -and $_.path -eq 'src/pch.h'
        })) {
            throw 'undeclared CommonLibF4 plugin PCH was not reported'
        }
        $safe = Invoke-Fo4HdrProjectAudit `
            -ProjectRoot $safeRoot `
            -AllowedReRoots @("src/game") `
            -AllowedReFiles @("src/main.cpp") `
            -AllowedF4seFiles @("src/main.cpp") `
            -PluginPchFiles @("src/pch.h")
        $wrongBoundary = $safe.summary.f4se_boundary_files -notcontains 'src/main.cpp' -or $safe.findings.Count
        if ($safe.summary.files_scanned -ne 2 -or $wrongBoundary) {
            $rendered = @($safe.findings | ForEach-Object { "$($_.code):$($_.path):$($_.line)" })
            throw (
                "safe fixture failed; findings=$([string]::Join(',', $rendered)); " +
                "files_scanned=$($safe.summary.files_scanned)"
            )
        }
}

function Test-PchSemanticUse {
    param([string]$SafeRoot)
        foreach ($semanticUse in @(
            'inline RE::TESForm* leakedGameObject = nullptr;',
            'inline F4SE::PluginHandle leakedPluginHandle = 0;',
            'inline REL::ID leakedRelocation{ 1 };',
            '#define LEAKED_RELOCATION REL::ID({ 1, 2 })',
            'inline :: RE :: TESForm* spacedGlobalGameObject = nullptr;',
            'using namespace RE;',
            'using namespace :: F4SE ;',
            'using namespace REL ;',
            'namespace game_types = RE;',
            'namespace extender_api = :: F4SE ;',
            'namespace relocations = :: REL ;'
        )) {
            [IO.File]::WriteAllText(
                (Join-Path $safeRoot "src\pch.h"),
                "#pragma once`n#include <RE/Fallout.h>`n#include <F4SE/F4SE.h>`n$semanticUse`n",
                [Text.UTF8Encoding]::new($false))
            $semanticPch = Invoke-Fo4HdrProjectAudit `
                -ProjectRoot $safeRoot `
                -AllowedReRoots @("src/game") `
                -AllowedReFiles @("src/main.cpp") `
                -PluginPchFiles @("src/pch.h")
            if (-not ($semanticPch.findings | Where-Object {
                $_.code -eq 'F4SE010' -and $_.path -eq 'src/pch.h'
            })) {
                throw "semantic engine use in a declared plugin PCH was not reported: $semanticUse"
            }
        }
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "src\pch.h"),
            "#pragma once`n#include <RE/Fallout.h>`n#include <F4SE/F4SE.h>`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::AppendAllText((Join-Path $safeRoot "selection.md"),
            '- `src/namespace-leak.cpp`' + "`n")
}

function Test-NamespaceSemanticUse {
    param([string]$SafeRoot)
        foreach ($namespaceUse in @(
            'using namespace RE;',
            'using namespace :: F4SE ;',
            'using namespace REL ;',
            'namespace game_types = :: RE ;',
            'namespace extender_api = F4SE;',
            'namespace relocations = :: REL ;'
        )) {
            [IO.File]::WriteAllText(
                (Join-Path $safeRoot "src\namespace-leak.cpp"),
                "$namespaceUse`n",
                [Text.UTF8Encoding]::new($false))
            $namespaceLeak = Invoke-Fo4HdrProjectAudit `
                -ProjectRoot $safeRoot `
                -AllowedReRoots @("src/game") `
                -AllowedReFiles @("src/main.cpp") `
                -PluginPchFiles @("src/pch.h")
            if (-not ($namespaceLeak.findings | Where-Object {
                $_.code -eq 'F4SE010' -and $_.path -eq 'src/namespace-leak.cpp'
            })) {
                throw "engine namespace use outside the declared boundary was not reported: $namespaceUse"
            }
        }
}

function Test-PchTextClassification {
    param([string]$SafeRoot)
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "src\namespace-leak.cpp"),
            "int namespaceLeakFixture = 0;`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "src\pch.h"),
            "#pragma once`n#include `"RE/Fallout.h`"`n#include `"F4SE/F4SE.h`"`ninline RE::TESForm* quotedIncludeLeak = nullptr;`n",
            [Text.UTF8Encoding]::new($false))
        $quotedIncludeSemanticPch = Invoke-Fo4HdrProjectAudit `
            -ProjectRoot $safeRoot `
            -AllowedReRoots @("src/game") `
            -AllowedReFiles @("src/main.cpp") `
            -PluginPchFiles @("src/pch.h")
        if (-not ($quotedIncludeSemanticPch.findings | Where-Object {
            $_.code -eq 'F4SE010' -and $_.path -eq 'src/pch.h'
        })) {
            throw 'semantic engine use after quoted PCH includes was not reported'
        }
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "src\pch.h"),
            @'
#pragma once
#include "RE/Fallout.h"
#include "F4SE/F4SE.h"
/*
using namespace RE;
namespace old_extender = ::F4SE;
#define OLD_RELOCATION REL::ID({ 1, 2 })
*/
namespace RENDER { struct Value; }
namespace F4SEXT {}
namespace RELATED {}
namespace renderer_types = RENDER;
namespace extender_types = F4SEXT;
using namespace RELATED;
inline RENDER :: Value* lookalike = nullptr;
constexpr auto description = "using namespace RE; namespace relocation = ::REL;";
constexpr auto rawDescription = R"(using namespace F4SE; namespace relocation = ::REL;)";
'@,
            [Text.UTF8Encoding]::new($false))
        $nonSemanticPch = Invoke-Fo4HdrProjectAudit `
            -ProjectRoot $safeRoot `
            -AllowedReRoots @("src/game") `
            -AllowedReFiles @("src/main.cpp") `
            -PluginPchFiles @("src/pch.h")
        if ($nonSemanticPch.findings | Where-Object {
            $_.code -eq 'F4SE010' -and $_.path -eq 'src/pch.h'
        }) {
            throw 'commented, string, or namespace-lookalike text in a declared plugin PCH was falsely reported'
        }
}

function Test-CommentedIncludes {
    param([string]$SafeRoot)
        [IO.File]::WriteAllText(
            (Join-Path $safeRoot "src\commented-includes.cpp"),
            "/*`n#include <RE/Fallout.h>`n#include `"F4SE/F4SE.h`"`n#include <REL/Relocation.h>`n*/`nint value = 0;`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::AppendAllText((Join-Path $safeRoot "selection.md"),
            '- `src/commented-includes.cpp`' + "`n")
        $commentedIncludes = Invoke-Fo4HdrProjectAudit `
            -ProjectRoot $safeRoot `
            -AllowedReRoots @("src/game") `
            -AllowedReFiles @("src/main.cpp") `
            -AllowedF4seFiles @("src/main.cpp") `
            -PluginPchFiles @("src/pch.h")
        if ($commentedIncludes.findings | Where-Object {
            $_.code -eq 'F4SE010' -and $_.path -eq 'src/commented-includes.cpp'
        }) {
            throw 'commented engine include was falsely reported as a live boundary use'
        }
}

function Invoke-Fo4HdrAuditSelfTest {
    $unsafeRoot = Join-Path ([IO.Path]::GetTempPath()) ("f4se-audit-" + [guid]::NewGuid().ToString("N"))
    $safeRoot = Join-Path ([IO.Path]::GetTempPath()) ("f4se-audit-safe-" + [guid]::NewGuid().ToString("N"))
    try {
        Test-UnsafeAuditFixture $UnsafeRoot
        Test-SafeAuditFixture $SafeRoot
        Test-PchSemanticUse $SafeRoot
        Test-NamespaceSemanticUse $SafeRoot
        Test-PchTextClassification $SafeRoot
        Test-CommentedIncludes $SafeRoot
        [Console]::Out.WriteLine("Self-test passed.")
        return 0
    } catch {
        [Console]::Error.WriteLine("Self-test failed: $($_.Exception.Message)")
        return 2
    } finally {
        foreach ($path in @($unsafeRoot, $safeRoot)) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
    }
}

if ($SelfTest) {
    $selfTestExitCode = Invoke-Fo4HdrAuditSelfTest
    exit $selfTestExitCode
}
if ([string]::IsNullOrWhiteSpace($Root)) {
    [Console]::Error.WriteLine("error: project root is required unless -SelfTest is used")
    exit 2
}
if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
    [Console]::Error.WriteLine("error: project root is not a directory: $Root")
    exit 2
}

try {
    $result = Invoke-Fo4HdrProjectAudit `
        -ProjectRoot $Root `
        -AllowedReRoots $ReRoot `
        -AllowedReFiles $AllowReFile `
        -AllowedF4seFiles $F4seFile `
        -PluginPchFiles $PluginPchFile
} catch {
    [Console]::Error.WriteLine("error: audit failed: $($_.Exception.Message)")
    exit 2
}

if ($Json) {
    [pscustomobject][ordered]@{
        summary = $result.summary
        findings = @($result.findings)
        disclaimer = "Heuristic review leads only; inspect against the pinned runtime and CommonLibF4 revision."
    } | ConvertTo-Json -Depth 8
} else {
    Write-Fo4HdrAuditText -Summary $result.summary -Findings @($result.findings)
}

$threshold = $script:SeverityRank[$FailOn]
if ($threshold -gt 0 -and @($result.findings | Where-Object { $script:SeverityRank[$_.severity] -ge $threshold }).Count) {
    exit 1
}
exit 0
