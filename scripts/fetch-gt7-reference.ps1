# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [switch]$AcceptLicense,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ExpectedSha256,

    [switch]$ImportReviewedSource
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

if (-not $AcceptLicense) {
    throw "Review the official MIT notice, then pass -AcceptLicense."
}

$url = "https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp"
$repoRoot = Get-Fo4HdrRepoRoot
$cacheDirectory =
    Join-Path $repoRoot "third_party\gt7\download-cache"
$cachePath =
    Join-Path $cacheDirectory "gt7_tone_mapping.cpp"

New-Item -ItemType Directory -Force $cacheDirectory |
    Out-Null

$response = Invoke-WebRequest `
    -Uri $url `
    -OutFile $cachePath `
    -PassThru `
    -MaximumRedirection 5 `
    -UseBasicParsing

$finalUri = $null
if ($response.BaseResponse.RequestMessage.RequestUri) {
    $finalUri =
        $response.BaseResponse.RequestMessage.RequestUri
} elseif ($response.BaseResponse.ResponseUri) {
    $finalUri = $response.BaseResponse.ResponseUri
}

if (-not $finalUri -or
    $finalUri.Host -ne "blog.selfshadow.com") {
    Remove-Item $cachePath -Force
    throw "GT7 download resolved to an unexpected host."
}

$text = Get-Content $cachePath -Raw
if ($text -notmatch "MIT License" -or
    $text -notmatch "Permission is hereby granted") {
    Remove-Item $cachePath -Force
    throw "Official source lacks the expected MIT notice."
}

$hash = Get-Sha256 $cachePath
if ($hash -ne $ExpectedSha256.ToLowerInvariant()) {
    throw (
        "GT7 source hash mismatch. Expected $ExpectedSha256; " +
        "downloaded $hash. Do not import until independently reviewed."
    )
}

Write-Host "Verified official GT7 sample."
Write-Host "SHA-256: $hash"

if ($ImportReviewedSource) {
    $upstreamDirectory =
        Join-Path $repoRoot "third_party\gt7\upstream"
    $upstreamPath =
        Join-Path $upstreamDirectory "gt7_tone_mapping.cpp"
    $lockPath =
        Join-Path $repoRoot "third_party\gt7\UPSTREAM.lock.json"

    New-Item -ItemType Directory -Force $upstreamDirectory |
        Out-Null
    Copy-Item $cachePath $upstreamPath -Force

    [ordered]@{
        sourceUrl = $url
        finalUrl = $finalUri.AbsoluteUri
        sha256 = $hash
        license = "MIT"
        importedUtc = [DateTime]::UtcNow.ToString("o")
        reviewed = $true
    } | ConvertTo-Json -Depth 8 |
        Set-Content $lockPath -Encoding utf8NoBOM

    Write-Host "Imported reviewed GT7 source and lock."
} else {
    Write-Warning (
        "Verified source remains in download-cache. " +
        "Review it, then rerun with -ImportReviewedSource."
    )
}
