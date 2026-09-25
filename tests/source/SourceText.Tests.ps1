# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../../scripts/internal/SourceText.ps1")

$cpp = @'
// precise REL::safe_write(); \
REL::safe_write();
/* precise */
const auto text = "precise // REL::safe_write();";
const auto raw = u8R"tag(text " precise REL::safe_write(); )tag";
const auto count = 1'000;
ImGui::BeginTabItem("Output");
precise float realValue;
REL::safe_write(realValue);
'@
$cpp = $cpp.Replace("`r`n", "`n")
foreach ($text in @($cpp, $cpp.Replace("`n", "`r`n"))) {
    foreach ($preserve in @($false, $true)) {
        $masked = Hide-Fo4HdrNonCodeText -Text $text -PreserveLiterals:$preserve
        if ($masked.Length -ne $text.Length -or
            [string]::Join('', [regex]::Matches($masked, '\r?\n').Value) -cne
            [string]::Join('', [regex]::Matches($text, '\r?\n').Value)) {
            throw "Source masking changed source offsets or line endings."
        }
        $expectedCount = if ($preserve) { 2 } else { 1 }
        if ([regex]::Matches($masked, '\bprecise\b').Count -ne $expectedCount -or
            [regex]::Matches($masked, 'REL::safe_write\(').Count -ne $expectedCount -or
            -not $masked.Contains("1'000")) {
            throw "C++ comment/string masking hid live code or exposed non-code."
        }
        if ($preserve -and -not $masked.Contains('BeginTabItem("Output")')) {
            throw "Literal-preserving source inspection lost the active UI label."
        }
    }
}

$powershell = @'
# Assert-Expected
<# Assert-Expected #>
$text = 'Assert-Expected # still a string'
Assert-Expected
'@
$masked = Hide-Fo4HdrNonCodeText -Text $powershell -Language PowerShell
if ([regex]::Matches($masked, 'Assert-Expected').Count -ne 1) {
    throw "PowerShell comments or strings were mistaken for commands."
}
$masked = Hide-Fo4HdrNonCodeText -Text $powershell -Language PowerShell -PreserveLiterals
if (-not $masked.Contains("'Assert-Expected # still a string'")) {
    throw "PowerShell string content was mistaken for a comment."
}

$lua = @'
-- path.join(os.projectdir(), "dist")
--[=[ path.join(os.projectdir(), "dist") ]=]
local example = [==[ path.join(os.projectdir(), "dist") ]==]
local url = "https://example.invalid/--path"
local stage = path.join(os.projectdir(), "dist")
'@
$masked = Hide-Fo4HdrNonCodeText -Text $lua -Language Lua -PreserveLiterals
if ([regex]::Matches($masked, 'path\.join').Count -ne 1 -or
    -not $masked.Contains('https://example.invalid/--path')) {
    throw "Lua comments/long strings affected the staging expression check."
}

Write-Host "Source text masking regression tests passed."
