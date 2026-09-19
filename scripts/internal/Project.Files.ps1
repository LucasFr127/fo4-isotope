# SPDX-License-Identifier: GPL-3.0-or-later

Set-StrictMode -Version Latest

function Read-Fo4HdrCanonicalUtf8Text {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Canonical text file does not exist: $Path"
    }

    $bytes = [IO.File]::ReadAllBytes($Path)
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    try {
        $offset = if (Test-Fo4HdrUtf8Bom $bytes) { 3 } else { 0 }
        $text = $utf8.GetString($bytes, $offset, $bytes.Length - $offset)
    }
    catch [Text.DecoderFallbackException] {
        throw "Canonical text is not valid UTF-8: $Path"
    }

    $canonical = $text.Replace("`r`n", "`n")
    if ($canonical.Contains("`r")) {
        throw (
            "Canonical text contains an unsupported carriage return: " +
            $Path
        )
    }

    if (-not $canonical.EndsWith("`n", [StringComparison]::Ordinal)) {
        $canonical += "`n"
    }

    return $canonical
}

function Get-Fo4HdrCanonicalTextSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $canonical = Read-Fo4HdrCanonicalUtf8Text $Path
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $hash = [Security.Cryptography.SHA256]::HashData(
        $utf8.GetBytes($canonical)
    )

    return [Convert]::ToHexString($hash).ToLowerInvariant()
}

function Write-Fo4HdrCanonicalUtf8Text {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    if ($Text.Contains("`r")) {
        throw "Canonical output must contain LF line endings only."
    }
    if (-not $Text.EndsWith(
        "`n",
        [StringComparison]::Ordinal
    )) {
        throw "Canonical output must end with a newline."
    }

    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Force $parent |
            Out-Null
    }

    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $temporaryPath =
        $Path + "." + [Guid]::NewGuid().ToString("N") + ".tmp"

    try {
        [IO.File]::WriteAllBytes(
            $temporaryPath,
            $utf8.GetBytes($Text)
        )
        Move-Item `
            -LiteralPath $temporaryPath `
            -Destination $Path `
            -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Get-Sha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $leaf = Split-Path -Leaf $Path
    if ([string]::Equals(
        $leaf,
        "LICENSE",
        [StringComparison]::OrdinalIgnoreCase
    ) -or [string]::Equals(
        $leaf,
        "EXCEPTIONS",
        [StringComparison]::OrdinalIgnoreCase
    )) {
        return Get-Fo4HdrCanonicalTextSha256 $Path
    }

    return (
        Get-FileHash -LiteralPath $Path -Algorithm SHA256
    ).Hash.ToLowerInvariant()
}

function Get-CanonicalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    return $fullPath.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
}

function Test-IsSameOrChildPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Candidate,

        [Parameter(Mandatory = $true)]
        [string]$Parent
    )

    $candidateFull = Get-CanonicalPath $Candidate
    $parentFull = Get-CanonicalPath $Parent
    $comparison = [StringComparison]::OrdinalIgnoreCase

    if ([string]::Equals(
        $candidateFull,
        $parentFull,
        $comparison
    )) {
        return $true
    }

    $prefix = $parentFull +
        [IO.Path]::DirectorySeparatorChar

    return $candidateFull.StartsWith(
        $prefix,
        $comparison)
}

function Write-Fo4HdrJsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Path,

        [string]$SchemaPath,

        [int]$Depth = 32
    )

    $json = $Value | ConvertTo-Json -Depth $Depth
    if ($SchemaPath) {
        if (-not (Test-Path $SchemaPath -PathType Leaf)) {
            throw "JSON schema does not exist: $SchemaPath"
        }
        if (-not ($json | Test-Json -SchemaFile $SchemaPath)) {
            throw "Generated JSON does not match schema: $SchemaPath"
        }
    }

    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Force $parent |
            Out-Null
    }

    $temporaryPath =
        $Path + "." + [Guid]::NewGuid().ToString("N") + ".tmp"

    try {
        Set-Content `
            -Path $temporaryPath `
            -Value $json `
            -Encoding utf8NoBOM
        Move-Item `
            -Path $temporaryPath `
            -Destination $Path `
            -Force
    }
    finally {
        if (Test-Path $temporaryPath) {
            Remove-Item $temporaryPath -Force
        }
    }
}

function Resolve-Fo4HdrChildPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Parent,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    if ([IO.Path]::IsPathRooted($RelativePath)) {
        throw "Expected a relative path, received: $RelativePath"
    }

    $parentFull = Get-CanonicalPath $Parent
    $candidate = Get-CanonicalPath (
        Join-Path $parentFull $RelativePath
    )

    if (-not (Test-IsSameOrChildPath $candidate $parentFull) -or
        [string]::Equals(
            $candidate,
            $parentFull,
            [StringComparison]::OrdinalIgnoreCase
        )) {
        throw "Path escapes the approved parent: $RelativePath"
    }

    return $candidate
}

function Test-Fo4HdrUtf8Bom {
    param([byte[]]$Bytes)
    return $Bytes.Length -ge 3 -and $Bytes[0] -eq 0xEF -and
        $Bytes[1] -eq 0xBB -and $Bytes[2] -eq 0xBF
}
