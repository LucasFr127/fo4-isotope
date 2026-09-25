# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
function Clear-Fo4HdrSourceRange {
    param([hashtable]$Scanner, [int]$End, [string[]]$Keep = @("`r", "`n"))
    $output = $Scanner.output
    for ($cursor = $Scanner.index; $cursor -lt $End; ++$cursor) {
        if ($output[$cursor] -notin $Keep) { $output[$cursor] = " " }
    }
}

function Hide-Fo4HdrPowerShellText {
    param([hashtable]$Scanner)
    $tokens = $null
    $parseErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseInput(
        $Scanner.Text, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Cannot inspect invalid PowerShell source: $($parseErrors[0].Message)" }
    $hiddenKinds = @("Comment")
    if (-not $Scanner.PreserveLiterals) {
        $hiddenKinds += @("StringLiteral", "StringExpandable", "HereStringLiteral", "HereStringExpandable")
    }
    foreach ($token in $tokens) {
        if ($token.Kind -notin $hiddenKinds) { continue }
        $Scanner.index = $token.Extent.StartOffset
        Clear-Fo4HdrSourceRange $Scanner $token.Extent.EndOffset
    }
}

function Read-Fo4HdrLuaOpening {
    param([hashtable]$Scanner)
    $isComment = $Scanner.Text.Substring($Scanner.index).StartsWith('--')
    if (-not $isComment -and $Scanner.Text[$Scanner.index] -ne '[') { return $false }
    $opening = $Scanner.index + $(if ($isComment) { 2 } else { 0 })
    $longBracket = [regex]::Match($Scanner.Text.Substring($opening), '^\[(=*)\[')
    if ($longBracket.Success) {
        Read-Fo4HdrLuaLongBracket $Scanner $longBracket $opening
        return $true
    }
    if (-not $isComment) { return $false }
    Read-Fo4HdrCommentOpening $Scanner 'line-comment'
    return $true
}

function Read-Fo4HdrLuaLongBracket {
    param([hashtable]$Scanner, [Text.RegularExpressions.Match]$LongBracket, [int]$Opening)
    $Scanner.rawEnd = "]" + $LongBracket.Groups[1].Value + "]"
    $openingEnd = $Opening + $LongBracket.Length
    Clear-Fo4HdrSourceRange $Scanner $openingEnd @()
    $Scanner.index = $openingEnd
    $Scanner.state = "raw-string"
}

function Read-Fo4HdrCommentOpening {
    param([hashtable]$Scanner, [string]$State)
    Clear-Fo4HdrSourceRange $Scanner ($Scanner.index + 2) @()
    $Scanner.index += 2
    $Scanner.state = $State
}

function Read-Fo4HdrCppOpening {
    param([hashtable]$Scanner)
    $prefix = $Scanner.Text.Substring($Scanner.index, [Math]::Min(2, $Scanner.Text.Length - $Scanner.index))
    if ($prefix -in @('//', '/*')) {
        Read-Fo4HdrCommentOpening $Scanner $(if ($prefix -eq '//') { 'line-comment' } else { 'block-comment' })
        return $true
    }
    if ($prefix -ne 'R"') { return $false }
    return Read-Fo4HdrCppRawOpening $Scanner
}

function Read-Fo4HdrCppRawOpening {
    param([hashtable]$Scanner)
    # Raw strings stay masked even for label checks: their bodies can contain arbitrary code examples.
    $searchLength = [Math]::Min($Scanner.Text.Length, $Scanner.index + 20) - ($Scanner.index + 2)
    $delimiterEnd = if ($searchLength -gt 0) { $Scanner.Text.IndexOf("(", $Scanner.index + 2, $searchLength) } else { -1 }
    if ($delimiterEnd -lt 0) { return $false }
    $delimiter = $Scanner.Text.Substring($Scanner.index + 2, $delimiterEnd - ($Scanner.index + 2))
    $Scanner.rawEnd = ")$delimiter`""
    Clear-Fo4HdrSourceRange $Scanner ($delimiterEnd + 1) @("`n")
    $Scanner.index = $delimiterEnd + 1
    $Scanner.state = "raw-string"
    return $true
}

function Test-Fo4HdrDigitSeparator {
    param([hashtable]$Scanner)
    if ($Scanner.Language -ne "Cpp" -or $Scanner.index -le 0) { return $false }
    $next = if ($Scanner.index + 1 -lt $Scanner.Text.Length) { $Scanner.Text[$Scanner.index + 1] } else { [char]0 }
    if (-not [char]::IsLetterOrDigit($next)) { return $false }
    return $Scanner.Text.Substring(0, $Scanner.index) -match '(?<!\w)[0-9][\w'']*$'
}

function Read-Fo4HdrCodeCharacter {
    param([hashtable]$Scanner)
    if (-not (Move-Fo4HdrNextOpening $Scanner)) { return }
    $consumed = if ($Scanner.Language -eq 'Lua') { Read-Fo4HdrLuaOpening $Scanner } else { Read-Fo4HdrCppOpening $Scanner }
    if ($consumed) { return }
    $character = $Scanner.Text[$Scanner.index]
    if ($character -eq "'" -and (Test-Fo4HdrDigitSeparator $Scanner)) {
        ++$Scanner.index
        return
    }
    if ($character -in @('"', "'")) { Read-Fo4HdrQuoteOpening $Scanner }
    ++$Scanner.index
}

function Move-Fo4HdrNextOpening {
    param([hashtable]$Scanner)
    $opening = $Scanner.Text.IndexOfAny($Scanner.OpeningCharacters, $Scanner.index)
    if ($opening -lt 0) {
        $Scanner.index = $Scanner.Text.Length
        return $false
    }
    $Scanner.index = $opening
    return $true
}

function Read-Fo4HdrQuoteOpening {
    param([hashtable]$Scanner)
    if (-not $Scanner.PreserveLiterals) { $Scanner.output[$Scanner.index] = " " }
    $Scanner.state = if ($Scanner.Text[$Scanner.index] -eq '"') { 'string' } else { 'char' }
}

function Test-Fo4HdrContinuedComment {
    param([hashtable]$Scanner)
    $previous = $Scanner.index - 1
    if ($previous -ge 0 -and $Scanner.Text[$previous] -eq "`r") { --$previous }
    if ($Scanner.Language -ne "Cpp" -or $previous -lt 0) { return $false }
    return $Scanner.Text[$previous] -eq "\"
}

function Read-Fo4HdrLineComment {
    param([hashtable]$Scanner)
    $end = $Scanner.Text.IndexOf("`n", $Scanner.index)
    $end = Get-Fo4HdrSourceSpanEnd $Scanner $end
    Clear-Fo4HdrSourceRange $Scanner $end
    $Scanner.index = $end
    if ($end -eq $Scanner.Text.Length) { return }
    if (-not (Test-Fo4HdrContinuedComment $Scanner)) { $Scanner.state = 'code' }
    ++$Scanner.index
}

function Get-Fo4HdrSourceSpanEnd {
    param([hashtable]$Scanner, [int]$MatchOffset)
    if ($MatchOffset -lt 0) { return $Scanner.Text.Length }
    return $MatchOffset
}

function Read-Fo4HdrDelimitedText {
    param([hashtable]$Scanner)
    $terminator = if ($Scanner.state -eq 'block-comment') { '*/' } else { $Scanner.rawEnd }
    $end = if ($terminator) { $Scanner.Text.IndexOf($terminator, $Scanner.index, [StringComparison]::Ordinal) } else { -1 }
    $contentEnd = Get-Fo4HdrSourceSpanEnd $Scanner $end
    Clear-Fo4HdrSourceRange $Scanner $contentEnd
    $Scanner.index = $contentEnd
    if ($end -lt 0) { return }
    Clear-Fo4HdrSourceRange $Scanner ($end + $terminator.Length) @("`n")
    $Scanner.index += $terminator.Length
    $Scanner.state = 'code'
    $Scanner.rawEnd = ''
}

function Read-Fo4HdrLiteralEscape {
    param([hashtable]$Scanner)
    if (-not $Scanner.PreserveLiterals) { $Scanner.output[$Scanner.index] = " " }
    ++$Scanner.index
    if ($Scanner.index -ge $Scanner.Text.Length) { return }
    if (-not $Scanner.PreserveLiterals) { Clear-Fo4HdrSourceRange $Scanner ($Scanner.index + 1) }
    ++$Scanner.index
}

function Read-Fo4HdrLiteral {
    param([hashtable]$Scanner)
    $terminator = if ($Scanner.state -eq "string") { '"' } else { "'" }
    $end = $Scanner.Text.IndexOfAny([char[]]@('\', $terminator), $Scanner.index)
    $end = Get-Fo4HdrSourceSpanEnd $Scanner $end
    if (-not $Scanner.PreserveLiterals) { Clear-Fo4HdrSourceRange $Scanner $end }
    $Scanner.index = $end
    if ($end -eq $Scanner.Text.Length) { return }
    $character = $Scanner.Text[$Scanner.index]
    if ($character -eq "\") {
        Read-Fo4HdrLiteralEscape $Scanner
        return
    }
    if ($character -eq $terminator) { $Scanner.state = 'code' }
    if (-not $Scanner.PreserveLiterals) { Clear-Fo4HdrSourceRange $Scanner ($Scanner.index + 1) }
    ++$Scanner.index
}

function Hide-Fo4HdrNonCodeText {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
        [ValidateSet("Cpp", "PowerShell", "Lua")][string]$Language = "Cpp",
        [switch]$PreserveLiterals
    )
    $scanner = @{
        Text = $Text; output = $Text.ToCharArray(); index = 0; state = 'code'; rawEnd = ''
        Language = $Language; PreserveLiterals = $PreserveLiterals
        OpeningCharacters = if ($Language -eq 'Lua') { [char[]]"-['`"" } else { [char[]]"/R'`"" }
    }
    if ($Language -eq 'PowerShell') {
        Hide-Fo4HdrPowerShellText $scanner
        return [string]::new($scanner.output)
    }
    $readers = @{
        'code' = ${function:Read-Fo4HdrCodeCharacter}
        'line-comment' = ${function:Read-Fo4HdrLineComment}
        'block-comment' = ${function:Read-Fo4HdrDelimitedText}
        'raw-string' = ${function:Read-Fo4HdrDelimitedText}
        'string' = ${function:Read-Fo4HdrLiteral}
        'char' = ${function:Read-Fo4HdrLiteral}
    }
    while ($scanner.index -lt $Text.Length) {
        & $readers[$scanner.state] $scanner
    }
    return [string]::new($scanner.output)
}
