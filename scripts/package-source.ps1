# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$OutputPath,

    [switch]$IncludeWorkingTree
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

function Assert-SourcePackagingWorktree {
    param([string]$RepoRoot, [switch]$IncludeWorkingTree)
    if (-not (Test-Path (Join-Path $repoRoot ".git"))) {
        throw "Source packaging requires an initialized Git repository."
    }

    $workingTreeStatus = @(git -C $repoRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw 'Unable to check source working-tree status.' }
    if ($workingTreeStatus.Count -and
        -not $IncludeWorkingTree) {
        throw "Source packaging requires a clean working tree."
    }
    if ($IncludeWorkingTree) {
        Write-Warning (
            "Packaging the current working tree for validation. " +
            "Do not publish it as release corresponding source; release " +
            "packages require a clean public source tag."
        )
    }

}

function Read-PackagingSourceManifest {
    param([string]$RepoRoot)
    $requiresLock =
        Join-Path $repoRoot "xmake-requires.lock"
    if (-not (Test-Path $requiresLock)) {
        throw "xmake-requires.lock is missing. Restore the approved dependency lock."
    }

    $sourceManifestPath =
        Join-Path $repoRoot "third_party\xmake-source-manifest.json"
    if (-not (Test-Path $sourceManifestPath)) {
        throw "Missing third_party/xmake-source-manifest.json."
    }

    $sourceManifest =
        Get-Content $sourceManifestPath -Raw |
        ConvertFrom-Json
    if (-not $sourceManifest.complete) {
        throw (
            "XMake package-source manifest is incomplete. " +
            "Collect and hash every linked package source first."
        )
    }

    if (-not $env:FO4HDR_SOURCE_CACHE) {
        throw "FO4HDR_SOURCE_CACHE is not set."
    }

    return $sourceManifest
}

$repoRoot = Get-Fo4HdrRepoRoot
& (Join-Path $PSScriptRoot "validate-manifests.ps1")
& (Join-Path $PSScriptRoot "license-audit.ps1") -Scope full

Assert-SourcePackagingWorktree $repoRoot -IncludeWorkingTree:$IncludeWorkingTree

$sourceManifest = Read-PackagingSourceManifest $repoRoot

if (-not $OutputPath) {
    $OutputPath =
        Join-Path `
            $repoRoot `
            "dist\source\Fallout4HDR-corresponding-source.zip"
}

function Copy-PackageSourceArtifact {
    param([psobject]$Artifact, [string]$PackageName, [string]$PackageSourceRoot)
    $archive = Resolve-Fo4HdrChildPath `
        -Parent $env:FO4HDR_SOURCE_CACHE `
        -RelativePath $artifact.File

    if (-not (Test-Path $archive -PathType Leaf)) {
        throw (
            "Missing XMake package " +
            $artifact.Kind +
            " archive: " +
            $archive
        )
    }

    $actualHash = Get-Sha256 $archive
    if ($actualHash -ne
        $artifact.Hash.ToLowerInvariant()) {
        throw (
            "XMake package " +
            $artifact.Kind +
            " hash mismatch for " +
            $PackageName
        )
    }

    $packageDestination = Resolve-Fo4HdrChildPath `
        -Parent $packageSourceRoot `
        -RelativePath $artifact.File

    Copy-Item `
        $archive `
        $packageDestination `
        -Force
}

function Get-CommittedGitlinkPath {
    param([string]$GitRoot, [string]$Line)
    $parts = $line -split '\s+', 2
    if ($parts.Count -ne 2) {
        throw "Unable to parse .gitmodules line: $line"
    }
    $path = $parts[1].Replace('\', '/')
    $treeEntry = @(
        git -C $GitRoot ls-tree HEAD -- ":(literal)$path"
    )
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect Git tree path: $path"
    }
    if ($treeEntry.Count -eq 1 -and
        $treeEntry[0] -match '^160000 commit [0-9a-f]{40}\s') {
        return $path
    }
}

function Export-GitSourceBatch {
    param([string]$GitRoot, [string]$DestinationRoot, [string[]]$Files)
    $archivePath = Join-Path `
        ([IO.Path]::GetTempPath()) `
        ("Fallout4HDR-git-archive-" + [Guid]::NewGuid() + ".tar")
    try {
        $arguments = @(
            "-C",
            $GitRoot,
            "archive",
            "--format=tar",
            "--output=$archivePath",
            "HEAD",
            "--"
        )
        $arguments += @(
            $Files |
                ForEach-Object { ":(literal)$_" }
        )
        & git @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to archive committed source in $GitRoot"
        }

        tar.exe -xf $archivePath -C $DestinationRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract committed source for $GitRoot"
        }
    }
    finally {
        if (Test-Path -LiteralPath $archivePath) {
            Remove-Item -LiteralPath $archivePath -Force
        }
    }
}

function Get-ArchiveRelativeFiles {
    param([string]$SourceDirectory)
    $relativeFiles =
        [Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem `
        $SourceDirectory `
        -Recurse `
        -File) {
        $relativeFiles.Add(
            [IO.Path]::GetRelativePath(
                $SourceDirectory,
                $file.FullName).Replace('\', '/')
        )
    }
    $relativeFiles.Sort([StringComparer]::Ordinal)

    return $relativeFiles.ToArray()
}

function Write-SourceZipEntry {
    param([IO.Compression.ZipArchive]$Zip, [string]$SourceDirectory, [string]$Relative, [DateTimeOffset]$FixedTimestamp)
    $file = Get-Item -LiteralPath (
        Join-Path `
            $SourceDirectory `
            $relative.Replace('/', '\')
    )

    $entry = $zip.CreateEntry(
        $relative,
        [IO.Compression.CompressionLevel]::Optimal)
    $entry.LastWriteTime = $fixedTimestamp

    $entryStream = $entry.Open()
    $fileStream = [IO.File]::OpenRead($file.FullName)
    try {
        $fileStream.CopyTo($entryStream)
    }
    finally {
        $fileStream.Dispose()
        $entryStream.Dispose()
    }
}

function Write-DeterministicZipEntries {
    param([IO.Compression.ZipArchive]$Zip, [string]$SourceDirectory)
    $fixedTimestamp =
        [DateTimeOffset]::new(
            1980,
            1,
            1,
            0,
            0,
            0,
            [TimeSpan]::Zero)

    $relativeFiles = @(Get-ArchiveRelativeFiles $SourceDirectory)

    foreach ($relative in $relativeFiles) {
        Write-SourceZipEntry $zip $SourceDirectory $relative $fixedTimestamp
    }
}

function Copy-PackageSourceArtifacts {
    param([psobject]$SourceManifest, [string]$PackageSourceRoot)
    foreach ($package in $sourceManifest.packages) {
        foreach ($artifact in @(
            [pscustomobject]@{
                Kind = "source"
                File = $package.sourceCacheFile
                Hash = $package.sourceSha256
            },
            [pscustomobject]@{
                Kind = "recipe"
                File = $package.recipeCacheFile
                Hash = $package.recipeSha256
            }
        )) {
            Copy-PackageSourceArtifact $artifact $package.name $packageSourceRoot
        }
    }

}

function Get-GitlinkPaths {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GitRoot
    )

    $gitModules = Join-Path $GitRoot ".gitmodules"
    if (-not (Test-Path -LiteralPath $gitModules -PathType Leaf)) {
        return @()
    }

    $pathLines = @(
        git -C $GitRoot config `
            --file .gitmodules `
            --get-regexp path
    )
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to enumerate submodules in $GitRoot"
    }

    $paths = [Collections.Generic.List[string]]::new()
    foreach ($line in $pathLines) {
        $path = Get-CommittedGitlinkPath $GitRoot $line
        if ($path) { $paths.Add($path) }
    }
    return $paths.ToArray()
}

function Group-CommittedSourceFiles {
    param([string[]]$RelativeFiles, [string[]]$SubmodulePaths)
    $directFiles = [Collections.Generic.List[string]]::new()
    $submoduleFiles = @{}

    foreach ($relative in $RelativeFiles) {
        $submodule = @($submodulePaths | Where-Object {
            $relative.StartsWith(
                $_ + "/",
                [StringComparison]::Ordinal)
        })
        if ($submodule.Count -gt 1) {
            throw "Ambiguous submodule ownership for source path: $relative"
        }
        if ($submodule.Count -eq 0) {
            $directFiles.Add($relative)
            continue
        }

        $submodulePath = $submodule[0]
        if (-not $submoduleFiles.ContainsKey($submodulePath)) {
            $submoduleFiles[$submodulePath] =
                [Collections.Generic.List[string]]::new()
        }
        $submoduleFiles[$submodulePath].Add(
            $relative.Substring($submodulePath.Length + 1)
        )
    }

    return @{ DirectFiles = $directFiles; SubmoduleFiles = $submoduleFiles }
}

function Export-CommittedSourceFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GitRoot,

        [Parameter(Mandatory = $true)]
        [string]$DestinationRoot,

        [Parameter(Mandatory = $true)]
        [string[]]$RelativeFiles
    )

    $groups = Group-CommittedSourceFiles $RelativeFiles @(Get-GitlinkPaths -GitRoot $GitRoot)
    $directFiles = $groups.DirectFiles
    $submoduleFiles = $groups.SubmoduleFiles
    for ($start = 0; $start -lt $directFiles.Count; $start += 100) {
        $last = [Math]::Min($start + 99, $directFiles.Count - 1)
        Export-GitSourceBatch $GitRoot $DestinationRoot $directFiles[$start..$last]
    }

    foreach ($submodulePath in $submoduleFiles.Keys) {
        $submoduleRoot = Join-Path $GitRoot $submodulePath
        if (-not (Test-Path -LiteralPath $submoduleRoot -PathType Container)) {
            throw "Uninitialized submodule: $submoduleRoot"
        }
        $submoduleDestination = Join-Path $DestinationRoot $submodulePath
        New-Item `
            -ItemType Directory `
            -Force `
            $submoduleDestination |
            Out-Null

        Export-CommittedSourceFiles `
            -GitRoot $submoduleRoot `
            -DestinationRoot $submoduleDestination `
            -RelativeFiles $submoduleFiles[$submodulePath].ToArray()
    }
}

function Copy-SourceTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DestinationRoot,

        [Parameter(Mandatory = $true)]
        [string[]]$RelativeFiles,

        [switch]$UseWorkingTree
    )

    if (-not $UseWorkingTree) {
        Export-CommittedSourceFiles `
            -GitRoot $repoRoot `
            -DestinationRoot $DestinationRoot `
            -RelativeFiles $RelativeFiles
        return
    }

    foreach ($relative in $RelativeFiles) {
        $source = Join-Path $repoRoot $relative
        $destination = Join-Path $DestinationRoot $relative
        New-Item `
            -ItemType Directory `
            -Force `
            (Split-Path -Parent $destination) |
            Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

function New-DeterministicZip {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDirectory,

        [Parameter(Mandatory = $true)]
        [string]$DestinationFile
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $destinationParent =
        Split-Path -Parent $DestinationFile
    New-Item `
        -ItemType Directory `
        -Force `
        $destinationParent |
        Out-Null

    if (Test-Path $DestinationFile) {
        Remove-Item $DestinationFile -Force
    }

    $stream = [IO.File]::Open(
        $DestinationFile,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)

    try {
        $zip = [IO.Compression.ZipArchive]::new(
            $stream,
            [IO.Compression.ZipArchiveMode]::Create,
            $false)

        try {
            Write-DeterministicZipEntries $zip $SourceDirectory
        }
        finally {
            $zip.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

$tempRoot =
    Join-Path `
        ([IO.Path]::GetTempPath()) `
        ("Fallout4HDR-source-" + [Guid]::NewGuid())
$stageRoot = Join-Path $tempRoot "Fallout4HDR"

try {
    New-Item -ItemType Directory -Force $stageRoot |
        Out-Null

    $sourceFiles = @(Get-Fo4HdrSourceTreeFiles -Root $repoRoot)
    Copy-SourceTree `
        -DestinationRoot $stageRoot `
        -RelativeFiles $sourceFiles `
        -UseWorkingTree:$IncludeWorkingTree

    $packageSourceRoot =
        Join-Path `
            $stageRoot `
            "third_party\xmake-package-sources"
    New-Item `
        -ItemType Directory `
        -Force `
        $packageSourceRoot |
        Out-Null

    Copy-PackageSourceArtifacts $sourceManifest $packageSourceRoot
    & (Join-Path $PSScriptRoot "license-audit.ps1") `
        -PackageRoot $stageRoot `
        -Scope full

    New-DeterministicZip `
        -SourceDirectory $tempRoot `
        -DestinationFile $OutputPath

    $hash = Get-Sha256 $OutputPath
    Set-Content `
        ($OutputPath + ".sha256") `
        ($hash + "  " + (Split-Path $OutputPath -Leaf)) `
        -Encoding ascii

    Write-Host "Corresponding source: $OutputPath"
    Write-Host "SHA-256:             $hash"
}
finally {
    if (Test-Path $tempRoot) {
        Remove-Item $tempRoot -Recurse -Force
    }
}
