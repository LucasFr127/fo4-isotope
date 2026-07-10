# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.0

param(
    [switch]$AcceptLicense,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSha256,
    [switch]$ImportReviewedSource
)

$ErrorActionPreference = "Stop"
if (-not $AcceptLicense) { throw "Review the official MIT notice, then pass -AcceptLicense." }
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, ".."))
$lock = Get-Content -LiteralPath ([IO.Path]::Combine($root, "third_party/gt7/UPSTREAM.lock.json")) -Raw | ConvertFrom-Json
$url = "https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp"
$hash = "94df7e7e310b9423ebb5aaaa29416f376ebbae66e0948fb259f8688314b29822"
if ($lock.sourceUrl -cne $url -or $lock.finalUrl -cne $url -or $lock.sha256 -cne $hash -or
    $lock.license -cne "MIT" -or $ExpectedSha256.ToLowerInvariant() -cne $hash) {
    throw "Unexpected reviewed GT7 source identity."
}
$cache = [IO.Path]::Combine($root, "build/gt7-reference/gt7_tone_mapping.cpp")
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($cache)) | Out-Null
$response = Invoke-WebRequest -Uri $url -OutFile $cache -PassThru -MaximumRedirection 5 -UseBasicParsing
$finalUri = $response.BaseResponse.RequestMessage.RequestUri
if (-not $finalUri -or $finalUri.AbsoluteUri -cne $lock.finalUrl) {
    [IO.File]::Delete($cache)
    throw "GT7 download resolved to an unexpected origin or path."
}
$text = [IO.File]::ReadAllText($cache)
if ($text -notmatch "MIT License" -or $text -notmatch "Permission is hereby granted" -or
    $text -notmatch "Copyright \(c\) 2025 Polyphony Digital Inc\.") {
    [IO.File]::Delete($cache)
    throw "Official source lacks the expected MIT notice."
}
if ((Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) {
    [IO.File]::Delete($cache)
    throw "GT7 source hash mismatch; no source imported."
}
if ($ImportReviewedSource) {
    $destination = [IO.Path]::Combine($root, "third_party/gt7/upstream/gt7_tone_mapping.cpp")
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    [IO.File]::Copy($cache, $destination, $true)
    Write-Host "Imported the exact reviewed GT7 source; source lock unchanged."
}
else { Write-Host "Verified GT7 source remains under build/gt7-reference; source lock unchanged." }
