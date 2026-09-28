# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArchiveA,

    [Parameter(Mandatory = $true)]
    [string]$ArchiveB,

    [ValidateSet("Git", "WorkingTree")]
    [string]$ExpectedTree = "Git"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

function Assert-SourceArchiveEntries {
    param([IO.Compression.ZipArchive]$Archive, [string[]]$ExpectedNames)
    $entryNames = @($archive.Entries | ForEach-Object { $_.FullName })
    if (Compare-Object `
        $entryNames `
        $ExpectedNames `
        -SyncWindow 0) {
        throw (
            "Source-package entries differ from the source selection plus " +
            "the linked package-source archives."
        )
    }

}

function Get-LinkedSourceArtifacts {
    param([psobject]$SourceManifest)
    $linkedArtifacts = [Collections.Generic.List[object]]::new()
    foreach ($package in $sourceManifest.packages) {
        $linkedArtifacts.Add([pscustomobject]@{
            Path = "third_party/xmake-package-sources/" +
                $package.sourceCacheFile
            Hash = $package.sourceSha256.ToLowerInvariant()
        })
        $linkedArtifacts.Add([pscustomobject]@{
            Path = "third_party/xmake-package-sources/" +
                $package.recipeCacheFile
            Hash = $package.recipeSha256.ToLowerInvariant()
        })
    }
    return $linkedArtifacts.ToArray()
}

function Assert-RepeatedSourceArchive {
    param([string]$First, [string]$Second)
    foreach ($path in @($first, $second)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Source archive does not exist: $path"
        }
    }

    $firstHash = Get-Sha256 $first
    $secondHash = Get-Sha256 $second
    if ($firstHash -ne $secondHash) {
        throw "Repeated source-package generation is not byte-identical."
    }
    return $firstHash
}

function ConvertFrom-GitNulRecords {
    param([byte[]]$Bytes)
    if ($bytes.Length -eq 0) {
        return @()
    }
    if ($bytes[$bytes.Length - 1] -ne 0) {
        throw "Git tree enumeration omitted its final NUL terminator."
    }

    return Read-GitNulRecords $bytes
}

function Read-GitNulRecords {
    param([byte[]]$Bytes)
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $records = [Collections.Generic.List[string]]::new()
    $start = 0
    for ($index = 0; $index -lt $bytes.Length; ++$index) {
        if ($bytes[$index] -ne 0) { continue }
        if ($index -eq $start) {
            throw "Git tree enumeration returned an empty record."
        }
        $records.Add($utf8.GetString($bytes, $start, $index - $start))
        $start = $index + 1
    }
    return $records.ToArray()
}

function Assert-SourceArchiveTimestamps {
    param([IO.Compression.ZipArchive]$Archive)
    foreach ($entry in $archive.Entries) {
        $time = $entry.LastWriteTime
        if ($time.ToString('yyyy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture) -cne '1980-01-01 00:00:00') {
            throw "Source-package entry has a non-deterministic timestamp: $($entry.FullName)"
        }
    }

}

function Assert-PackagedSourceBytes {
    param([IO.Compression.ZipArchive]$Archive, [string]$ReferenceRoot, [string[]]$InventoryFiles, [string]$ExpectedTree)
    foreach ($relative in $inventoryFiles) {
        $entryName = "FO4Isotope/$relative"
        $archiveHash = Get-Fo4HdrZipEntrySha256 $archive $entryName
        $referenceHash = (Get-FileHash `
            -LiteralPath (Join-Path $referenceRoot $relative) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($archiveHash -ne $referenceHash) {
            throw "Packaged source bytes differ from $ExpectedTree bytes: $entryName"
        }
    }

}

function Assert-PackagedDependencyBytes {
    param([IO.Compression.ZipArchive]$Archive, [object[]]$LinkedArtifacts)
    foreach ($artifact in $linkedArtifacts) {
        $entryName = "FO4Isotope/$($artifact.Path)"
        if ((Get-Fo4HdrZipEntrySha256 $archive $entryName) -ne
            $artifact.Hash) {
            throw "Packaged linked source hash differs from its manifest: $entryName"
        }
    }
}

function Get-GitNulPathList {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GitRoot,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "git"
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.ArgumentList.Add("-C")
    $startInfo.ArgumentList.Add($GitRoot)
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $output = [IO.MemoryStream]::new()
    try {
        if (-not $process.Start()) {
            throw "Unable to start Git tree enumeration."
        }
        $copyTask = $process.StandardOutput.BaseStream.CopyToAsync($output)
        $errorTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        [void]$copyTask.GetAwaiter().GetResult()
        $errorText = $errorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "Git tree enumeration failed: $($errorText.Trim())"
        }
        $bytes = $output.ToArray()
    }
    finally {
        $output.Dispose()
        $process.Dispose()
    }

    return ConvertFrom-GitNulRecords $bytes
}

function Export-GitReferenceTree {
    param(
        [Parameter(Mandatory = $true)][string]$GitRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][string[]]$RelativeFiles
    )

    $archivePath = Join-Path `
        ([IO.Path]::GetTempPath()) `
        ("FO4Isotope-reference-" + [Guid]::NewGuid() + ".tar")
    try {
        git -C $GitRoot archive `
            --format=tar `
            --output=$archivePath `
            HEAD
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to export reference Git tree: $GitRoot"
        }
        tar.exe -xf $archivePath -C $DestinationRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract reference Git tree: $GitRoot"
        }
    }
    finally {
        if (Test-Path -LiteralPath $archivePath) {
            Remove-Item -LiteralPath $archivePath -Force
        }
    }

    foreach ($record in Get-GitNulPathList `
        -GitRoot $GitRoot `
        -Arguments @("ls-tree", "-rz", "HEAD")) {
        if ($record -notmatch
            '^160000 commit [0-9a-f]{40}\t(?<path>.+)$') {
            if ($record -match '^[0-9]{6} (blob|tree) [0-9a-f]{40}\t.+$') {
                continue
            }
            throw "Unable to parse Git tree record: $record"
        }

        $gitlinkPath = $Matches.path
        $childFiles = @($RelativeFiles | Where-Object {
            $_.StartsWith(
                $gitlinkPath + "/",
                [StringComparison]::Ordinal)
        })
        if ($childFiles.Count -eq 0) {
            continue
        }

        $childDestination = Join-Path $DestinationRoot $gitlinkPath
        New-Item -ItemType Directory -Force $childDestination |
            Out-Null
        Export-GitReferenceTree `
            -GitRoot (Join-Path $GitRoot $gitlinkPath) `
            -DestinationRoot $childDestination `
            -RelativeFiles @($childFiles | ForEach-Object {
                $_.Substring($gitlinkPath.Length + 1)
            })
    }
}

$first = [IO.Path]::GetFullPath($ArchiveA)
$second = [IO.Path]::GetFullPath($ArchiveB)
$firstHash = Assert-RepeatedSourceArchive $first $second

$inventoryFiles = @(Get-Fo4HdrSourceTreeFiles -Root $repoRoot)
$sourceManifest = Get-Content `
    (Join-Path $repoRoot "third_party\xmake-source-manifest.json") `
    -Raw |
    ConvertFrom-Json
$linkedArtifacts = @(Get-LinkedSourceArtifacts $sourceManifest)

$expectedNames = [Collections.Generic.List[string]]::new()
foreach ($relative in $inventoryFiles) {
    $expectedNames.Add("FO4Isotope/$relative")
}
foreach ($artifact in $linkedArtifacts) {
    $expectedNames.Add("FO4Isotope/$($artifact.Path)")
}
$expectedNames.Sort([StringComparer]::Ordinal)

$archive = Open-Fo4HdrValidatedZipArchive $first -RequiredRoot "FO4Isotope"
$referenceRoot = $repoRoot
$referenceTemp = $null
try {
    Assert-SourceArchiveEntries $archive $expectedNames.ToArray()

    Assert-SourceArchiveTimestamps $archive
    if ($ExpectedTree -eq "Git") {
        $referenceTemp = Join-Path `
            ([IO.Path]::GetTempPath()) `
            ("FO4Isotope-reference-" + [Guid]::NewGuid())
        $referenceRoot = Join-Path $referenceTemp "FO4Isotope"
        New-Item -ItemType Directory -Force $referenceRoot |
            Out-Null
        Export-GitReferenceTree `
            -GitRoot $repoRoot `
            -DestinationRoot $referenceRoot `
            -RelativeFiles $inventoryFiles
    }

    Assert-PackagedSourceBytes $archive $referenceRoot $inventoryFiles $ExpectedTree
    Assert-PackagedDependencyBytes $archive $linkedArtifacts
}
finally {
    $archive.Dispose()
    if ($referenceTemp -and (Test-Path -LiteralPath $referenceTemp)) {
        Remove-Item -LiteralPath $referenceTemp -Recurse -Force
    }
}

$expectedDescription = if ($ExpectedTree -eq "Git") {
    "Git export"
} else {
    "working-tree"
}
Write-Host (
    "Deterministic corresponding-source regression tests passed " +
    "against exact selected $expectedDescription bytes: $firstHash"
)
