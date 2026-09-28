# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

function Test-Fo4HdrOfficialGt7Origin {
    param([string]$RelativePath, [string]$Text, [string]$LicenseExpression)

    if ($RelativePath -notin @('src/tonemap/Gt7.cpp', 'shaders/tonemap/Gt7.hlsli') -or
        $LicenseExpression -cne 'MIT') {
        return $false
    }
    $origin = [regex]::Match(
        $Text,
        '(?im)(?:Upstream:|SPDX-SnippetComment:\s*adapted from)\s*' +
        [regex]::Escape($officialGt7Url) +
        '\s+sha256=(?<hash>[0-9a-f]{64})(?:\s|$)'
    )
    return $origin.Success -and
        $origin.Groups['hash'].Value -ceq [string]$gt7Authority.expectedSha256
}

function Assert-Fo4HdrImportedSource {
    param([string]$RelativePath, [string]$Text, [string]$LicenseExpression,
        [bool]$Snippet)

    # REUSE-IgnoreStart
    if ($Snippet) {
        $copyright = $Text -match '(?im)^\s*(?://|#|--|\*)?\s*SPDX-SnippetCopyrightText:\s*\S'
        $gitOrigin = $Text -match
        '(?im)SPDX-SnippetComment:.*\b[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}/\S+'
    } else {
        $copyright = $Text -match '(?im)^\s*(?://|#|--|\*)?\s*(?:Copyright\s*(?:\(c\))?|SPDX-FileCopyrightText:)\s*\S'
        $gitOrigin = $Text -match
        '(?im)Upstream:\s*[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}/\S+'
    }
    # REUSE-IgnoreEnd
    if ($copyright -and -not $gitOrigin) {
        $gitOrigin = Test-Fo4HdrOfficialGt7Origin $RelativePath $Text $LicenseExpression
    }
    if (-not $copyright -or -not $gitOrigin) {
        throw "Imported source lacks retained copyright or pinned origin: $RelativePath"
    }
    if ($LicenseExpression -match '\b(?:GPL-3\.0-or-later|Apache-2\.0)\b' -and
        $Text -notmatch '(?i)\bmodif(?:ied|ication|ications)\b') {
        throw "Imported source lacks a modification notice: $RelativePath"
    }
}

function Complete-Fo4HdrLicenseSnippet {
    param([string]$RelativePath, [Collections.Generic.Stack[object]]$Snippets,
        [string]$LicenseLine)

    if ($Snippets.Count -eq 0) {
        throw "Unmatched SPDX-SnippetEnd: $RelativePath"
    }
    $snippetText = [string]::Join("`n", $Snippets.Pop())
    $snippetExpressions = @(
        [regex]::Matches($snippetText, '(?m)' + $LicenseLine) |
        ForEach-Object { $_.Groups['expression'].Value.Trim() }
    )
    Assert-Fo4HdrImportedSource $RelativePath $snippetText (
        [string]::Join(' ', $snippetExpressions)
    ) $true
}

function Assert-Fo4HdrProductionSourceLicense {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $lines = [regex]::Split($Text, '\r?\n')
    $snippets = [Collections.Generic.Stack[object]]::new()
    $fileExpressions = [Collections.Generic.List[string]]::new()
    # REUSE-IgnoreStart
    $licenseLine = '^\s*(?://|#|--|\*)?\s*SPDX-License-Identifier:\s*(?<expression>.*?)\s*(?:\*/)?\s*$'
    $beginLine = '^\s*(?://|#|--|\*)?\s*SPDX-SnippetBegin\s*(?:\*/)?\s*$'
    $endLine = '^\s*(?://|#|--|\*)?\s*SPDX-SnippetEnd\s*(?:\*/)?\s*$'
    # REUSE-IgnoreEnd

    foreach ($line in $lines) {
        if ($line -match $beginLine) {
            $snippets.Push([Collections.Generic.List[string]]::new())
            continue
        }
        if ($line -match $endLine) {
            Complete-Fo4HdrLicenseSnippet $RelativePath $snippets $licenseLine
            continue
        }
        if ($snippets.Count -gt 0) {
            $snippets.Peek().Add($line)
            continue
        }
        $license = [regex]::Match($line, $licenseLine)
        if ($license.Success) {
            $fileExpressions.Add($license.Groups['expression'].Value.Trim())
        }
    }
    if ($snippets.Count -gt 0) { throw "Unclosed SPDX snippet: $RelativePath" }

    $importedExpressions = @($fileExpressions | Where-Object { $_ -cne $projectSpdxId })
    if ($importedExpressions.Count -gt 0) {
        Assert-Fo4HdrImportedSource $RelativePath $Text (
            [string]::Join(' ', $importedExpressions)
        ) $false
    }
}

function Assert-Fo4HdrRejectedSourceLicense {
    param([Collections.IDictionary]$Case)

    $rejected = $false
    try { Assert-Fo4HdrProductionSourceLicense $Case.path $Case.text }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Source-license self-test accepted $($Case.name)." }
}

function Assert-Fo4HdrProductionSourceLicenseSelfTests {
    $commit = '0123456789abcdef0123456789abcdef01234567'
    $spdx = 'SPDX-'
    $project = "// ${spdx}License-Identifier: $projectSpdxId"
    $imported = @(
        "// ${spdx}License-Identifier: GPL-3.0-or-later",
        '// Copyright (c) Upstream',
        "// Upstream: org/repo@$commit/source.cpp",
        '// Modified by FO4Isotope'
    ) -join "`n"
    $snippet = @(
        $project,
        "// ${spdx}SnippetBegin",
        "// ${spdx}License-Identifier: Apache-2.0",
        "// ${spdx}SnippetCopyrightText: Copyright Upstream",
        "// ${spdx}SnippetComment: modified from org/repo@$commit/source.cpp",
        "// ${spdx}SnippetEnd"
    ) -join "`n"
    $gt7 = @(
        "// ${spdx}License-Identifier: MIT",
        '// Copyright (c) Polyphony Digital',
        "// Upstream: $officialGt7Url sha256=$($gt7Authority.expectedSha256)"
    ) -join "`n"
    $nested = $snippet.Replace(
        "// ${spdx}SnippetEnd",
        @(
            "// ${spdx}SnippetBegin",
            "// ${spdx}License-Identifier: MIT",
            "// ${spdx}SnippetCopyrightText: Copyright Other Upstream",
            "// ${spdx}SnippetComment: adapted from other/repo@$commit/source.cpp",
            "// ${spdx}SnippetEnd",
            "// ${spdx}SnippetEnd"
        ) -join "`n"
    )
    foreach ($case in @(
        @{ path = 'src/project.cpp'; text = $project },
        @{ path = 'src/imported.cpp'; text = $imported },
        @{ path = 'src/snippet.cpp'; text = $snippet },
        @{ path = 'src/nested.cpp'; text = $nested },
        @{ path = 'src/siblings.cpp'; text = $snippet + "`n" + $snippet },
        @{ path = 'shaders/tonemap/Gt7.hlsli'; text = $gt7 }
    )) {
        Assert-Fo4HdrProductionSourceLicense $case.path $case.text
    }
    foreach ($case in @(
        @{ name = 'unclosed snippet'; path = 'src/snippet.cpp';
            text = $snippet.Replace("// ${spdx}SnippetEnd", '') },
        @{ name = 'unmatched snippet end'; path = 'src/snippet.cpp';
            text = $project + "`n// ${spdx}SnippetEnd" },
        @{ name = 'missing imported origin'; path = 'src/imported.cpp';
            text = $imported.Replace("@$commit/", '@main/') },
        @{ name = 'missing imported copyright'; path = 'src/imported.cpp';
            text = $imported.Replace('// Copyright (c) Upstream', '') },
        @{ name = 'missing modification notice'; path = 'src/imported.cpp';
            text = $imported.Replace('// Modified by FO4Isotope', '') },
        @{ name = 'missing snippet origin'; path = 'src/snippet.cpp';
            text = $snippet.Replace("@$commit/", '@main/') },
        @{ name = 'missing nested snippet origin'; path = 'src/nested.cpp';
            text = $nested.Replace("other/repo@$commit/", 'other/repo@main/') },
        @{ name = 'missing outer snippet origin'; path = 'src/nested.cpp';
            text = $nested.Replace("modified from org/repo@$commit/", 'modified from org/repo@main/') },
        @{ name = 'wrong GT7 hash'; path = 'shaders/tonemap/Gt7.hlsli';
            text = $gt7.Replace([string]$gt7Authority.expectedSha256, '0' * 64) }
    )) {
        Assert-Fo4HdrRejectedSourceLicense $case
    }
}
