# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

Set-StrictMode -Version Latest

function Get-Fo4HdrSourceTreeFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [string]$InventoryPath
    )

    $rootPath = [IO.Path]::TrimEndingDirectorySeparator(
        [IO.Path]::GetFullPath($Root)
    )
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) {
        throw "Source-tree root does not exist: $rootPath"
    }

    $entries = @(Get-Fo4HdrSourceInventoryEntries $rootPath $InventoryPath)

    $rootPrefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    $files = [Collections.Generic.List[string]]::new()
    $selection = [pscustomobject]@{ Root = $rootPath; Prefix = $rootPrefix; Seen = $seen; Files = $files }
    foreach ($line in $entries) { Add-Fo4HdrSourceInventoryLine $line $selection }

    if ($files.Count -eq 0) {
        throw "Source selection contains no files: $rootPath"
    }
    $result = $files.ToArray()
    [Array]::Sort($result, [StringComparer]::Ordinal)
    return $result
}

function Get-Fo4HdrZipEntrySha256 {
    param(
        [Parameter(Mandatory = $true)]
        [IO.Compression.ZipArchive]$Archive,

        [Parameter(Mandatory = $true)]
        [string]$EntryName
    )

    $entry = $Archive.GetEntry($EntryName)
    if (-not $entry) {
        throw "Archive is missing required entry: $EntryName"
    }

    $stream = $entry.Open()
    try {
        $hash = [Security.Cryptography.SHA256]::Create()
        try {
            return [Convert]::ToHexString(
                $hash.ComputeHash($stream)
            ).ToLowerInvariant()
        }
        finally {
            $hash.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Read-Fo4HdrZipEntryUtf8Text {
    param(
        [Parameter(Mandatory = $true)]
        [IO.Compression.ZipArchive]$Archive,

        [Parameter(Mandatory = $true)]
        [string]$EntryName
    )

    $entry = $Archive.GetEntry($EntryName)
    if (-not $entry) {
        throw "Archive is missing required entry: $EntryName"
    }

    $stream = $entry.Open()
    try {
        $reader = [IO.StreamReader]::new(
            $stream,
            [Text.UTF8Encoding]::new($false, $true),
            $true,
            4096,
            $true
        )
        try {
            return $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Open-Fo4HdrValidatedZipArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [string]$RequiredRoot
    )

    Add-Type -AssemblyName System.IO.Compression
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $names = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase
        )

        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace("\", "/")
            if ($RequiredRoot -and -not $name.StartsWith(
                $RequiredRoot + "/", [StringComparison]::Ordinal)) {
                throw "Archive entry is outside the required $RequiredRoot/ root: $name"
            }
            Assert-Fo4HdrArchiveEntryName $name
            if (-not $names.Add($name)) {
                throw "Archive contains a duplicate path: $name"
            }
        }

        return $archive
    }
    catch {
        $archive.Dispose()
        throw
    }
}

function Get-Fo4HdrValidatedTarEntryBytes {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$EntryName
    )

    $archivePath = [IO.Path]::GetFullPath($Path)
    $listing = Get-Fo4HdrTarListing $archivePath

    $names = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($rawName in @($listing -split "`r?`n" | Where-Object { $_ })) {
        $name = $rawName.Replace("\", "/")
        Assert-Fo4HdrArchiveEntryName $name
        if (-not $names.Add($name)) {
            throw "Archive contains a duplicate path: $name"
        }
    }
    if (-not $names.Contains($EntryName)) {
        throw "Archive is missing required entry: $EntryName"
    }

    return Read-Fo4HdrTarEntry $archivePath $EntryName

}

function Get-Fo4HdrSourceArchiveEntrySha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Format,
        [Parameter(Mandatory = $true)][string]$EntryName
    )
    if ($Format -eq "zip") {
        $archive = Open-Fo4HdrValidatedZipArchive $Path
        try { return Get-Fo4HdrZipEntrySha256 $archive $EntryName }
        finally { $archive.Dispose() }
    }
    if ($Format -ne "tar.gz") { throw "Unsupported source archive format: $Format" }
    $bytes = Get-Fo4HdrValidatedTarEntryBytes $Path $EntryName
    $hash = [Security.Cryptography.SHA256]::HashData($bytes)
    return [Convert]::ToHexString($hash).ToLowerInvariant()
}

function Test-Fo4HdrDependencySources {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactRoot
    )

    $dependencyLock = Read-Fo4HdrDependencyLock $RepoRoot
    $versions = $dependencyLock.Versions

    $manifestPath = Join-Path `
        $RepoRoot `
        "third_party\xmake-source-manifest.json"
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    if (-not $manifest.complete) {
        throw "The XMake package-source manifest is incomplete."
    }

    $packages = @($manifest.packages)
    if ($packages.Count -ne 2 -or
        (Compare-Object @($packages.name | Sort-Object) @("imgui", "spdlog"))) {
        throw (
            "The linked XMake source manifest must contain exactly ImGui and spdlog; " +
            "CMake and Ninja in the XMake lock are build tools, not linked " +
            "libraries."
        )
    }

    Assert-Fo4HdrSpdlogSources $packages $versions $dependencyLock.Spdlog $ArtifactRoot

    Assert-Fo4HdrImguiSources $packages $versions $ArtifactRoot

    Assert-Fo4HdrDependencyNotices $RepoRoot
    Write-Host "Validated locked ImGui and spdlog source, recipe, license, and notice identities."
}

function Get-Fo4HdrSourceInventoryEntries {
    param([string]$RootPath, [string]$InventoryPath)
    $isCheckout = Test-Path -LiteralPath (Join-Path $rootPath '.git')
    $InventoryPath = Resolve-Fo4HdrSourceInventorySetting $rootPath $InventoryPath $isCheckout
    if ($InventoryPath) {
        $entries = @(Read-Fo4HdrSourceInventory $rootPath $InventoryPath)
    } elseif ($isCheckout) {
        $entries = @(Get-Fo4HdrTrackedSourceEntries $rootPath)
    } else {
        $entries = @(Get-Fo4HdrExtractedSourceEntries $rootPath)
    }

    return $entries
}

function Add-Fo4HdrSourceInventoryLine {
    param([string]$Line, [psobject]$Selection)
    if ($line -notmatch '^- `(?<path>[^`]+)`(?<submodule> \(submodule\))?$') {
        if ($line.StartsWith('- `', [StringComparison]::Ordinal)) {
            throw "Malformed source-tree inventory entry: $line"
        }
        return
    }

    $relative = $Matches.path
    $isSubmodule = [bool]$Matches['submodule']
    $fullPath = Resolve-Fo4HdrSourceInventoryPath $relative $Selection

    if ($isSubmodule) {
        Add-Fo4HdrSourceSubmodule $relative $fullPath $Selection
        return
    }

    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Source-tree inventory file does not exist: $relative"
    }
    $Selection.Files.Add($relative)
}

function Resolve-Fo4HdrSourceInventoryPath {
    param([string]$Relative, [psobject]$Selection)
    Assert-Fo4HdrSourceInventoryName $relative
    if (-not $Selection.Seen.Add($relative)) {
        throw "Duplicate source-tree inventory path: $relative"
    }

    $fullPath = [IO.Path]::GetFullPath((Join-Path $Selection.Root $relative))
    if (-not $fullPath.StartsWith(
            $Selection.Prefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe source-tree inventory path: $relative"
    }

    return $fullPath
}

function Add-Fo4HdrSourceSubmodule {
    param([string]$Relative, [string]$FullPath, [psobject]$Selection)
    Assert-Fo4HdrSourceSubmodule $relative $fullPath $Selection

    $children = @(Get-Fo4HdrSubmoduleSourceFiles $relative $fullPath $Selection)
    foreach ($child in $children) {
        Add-Fo4HdrSubmoduleSourceFile $child $relative $fullPath $Selection
    }

}

function Read-Fo4HdrDependencyLock {
    param([string]$RepoRoot)
    $lockPath = Join-Path $RepoRoot "xmake-requires.lock"
    if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
        throw "xmake-requires.lock is required for the approved dependency graph."
    }

    $versions = Get-Content `
        (Join-Path $RepoRoot "versions.lock.json") `
        -Raw |
        ConvertFrom-Json
    $lockHash = Get-Sha256 $lockPath
    if ($lockHash -ne
        $versions.xmake.requiresLockSha256.ToLowerInvariant()) {
        throw "xmake-requires.lock differs from versions.lock.json."
    }

    $lockText = Get-Content $lockPath -Raw
    Assert-Fo4HdrLockedPackageSet $lockText

    $lockedSpdlog = [regex]::Match(
        $lockText,
        '(?ms)^ {8}\["spdlog (?<version>v[0-9.]+)#[0-9a-f]+"\]\s*=\s*\{(?<body>.*?)(?=^ {8}\["|^ {4}\})')
    if (-not $lockedSpdlog.Success) {
        throw "xmake-requires.lock does not contain spdlog."
    }

    return [pscustomobject]@{ Versions = $versions; Spdlog = $lockedSpdlog }
}

function Assert-Fo4HdrSpdlogSources {
    param([object[]]$Packages, [psobject]$Versions, [Text.RegularExpressions.Match]$LockedSpdlog, [string]$ArtifactRoot)
    $package = @($packages | Where-Object name -eq "spdlog")[0]
    Assert-Fo4HdrSpdlogSourceIdentity $package $versions $lockedSpdlog

    $artifacts = Get-Fo4HdrPackageSourceArtifacts $package $ArtifactRoot
    $sourcePath = $artifacts.Source
    $recipePath = $artifacts.Recipe
    if ((Get-Sha256 $sourcePath) -ne
        $package.sourceSha256.ToLowerInvariant()) {
        throw "The cached spdlog source archive hash differs from its manifest."
    }
    if ((Get-Sha256 $recipePath) -ne
        $package.recipeSha256.ToLowerInvariant()) {
        throw "The cached spdlog recipe archive hash differs from its manifest."
    }

    Assert-Fo4HdrSpdlogArchiveContents $package $sourcePath $recipePath

}

function Assert-Fo4HdrImguiSources {
    param([object[]]$Packages, [psobject]$Versions, [string]$ArtifactRoot)
    $imgui = @($packages | Where-Object name -eq "imgui")[0]
    Assert-Fo4HdrImguiSourceIdentity $imgui $versions.xmake.packages.imgui
    $artifacts = Get-Fo4HdrPackageSourceArtifacts $imgui $ArtifactRoot
    $imguiSourcePath = $artifacts.Source
    $imguiRecipePath = $artifacts.Recipe
    if ((Get-Sha256 $imguiSourcePath) -ne $imgui.sourceSha256 -or
        (Get-Sha256 $imguiRecipePath) -ne $imgui.recipeSha256) {
        throw "The cached ImGui source or recipe hash differs from its manifest."
    }
    Assert-Fo4HdrImguiArchiveContents $imgui $imguiSourcePath $imguiRecipePath

}

function Assert-Fo4HdrDependencyNotices {
    param([string]$RepoRoot)
    $notices = Get-Content `
        (Join-Path $RepoRoot "THIRD_PARTY_NOTICES.md") `
        -Raw
    foreach ($noticeIdentity in @(
        "spdlog",
        "Copyright (c) 2016 Gabi Melman",
        "Dear ImGui",
        "Copyright (c) 2014-2026 Omar Cornut",
        "MIT"
    )) {
        if ($notices -notlike "*$noticeIdentity*") {
            throw "THIRD_PARTY_NOTICES.md is missing: $noticeIdentity"
        }
    }
}

function Assert-Fo4HdrSourceInventoryName {
    param([string]$Relative)
    $segments = $relative.Split('/')
    $unsafe = [IO.Path]::IsPathRooted($relative) -or
        $relative.IndexOfAny([char[]]'\:') -ge 0 -or
        @($segments | Where-Object { $_ -in @('', '.', '..', '.git') }).Count
    if ($unsafe) {
        throw "Unsafe source-tree inventory path: $relative"
    }
}

function Get-Fo4HdrTrackedSourceEntries {
    param([string]$RootPath)
    $tracked = @(git -c core.quotePath=false -C $rootPath ls-files --stage)
    if ($LASTEXITCODE -ne 0) { throw 'Unable to enumerate tracked source files.' }
    $entries = @($tracked | ForEach-Object {
        if ($_ -notmatch '^(?<mode>[0-9]{6}) [0-9a-f]+ 0\t(?<path>.+)$') {
            throw "Unmerged or invalid Git source entry: $_"
        }
        $suffix = if ($Matches.mode -eq '160000') { ' (submodule)' } else { '' }
        '- `' + $Matches.path + '`' + $suffix
    })
    return $entries
}

function Get-Fo4HdrExtractedSourceEntries {
    param([string]$RootPath)
    # Rebuilds add outputs to extracted archives; these are not source inputs.
    $entries = @(Get-ChildItem -LiteralPath $rootPath -Force | Where-Object {
        $_.Name -notin @('build', 'dist', '.xmake', 'local.env.ps1')
    } | ForEach-Object {
        if ($_.PSIsContainer) { Get-ChildItem -LiteralPath $_.FullName -Force -Recurse -File }
        else { $_ }
    } | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($rootPath, $_.FullName).Replace('\', '/')
        if (-not $relative.StartsWith('third_party/xmake-package-sources/',
                [StringComparison]::OrdinalIgnoreCase)) {
            '- `' + $relative + '`'
        }
    })
    return $entries
}

function Assert-Fo4HdrSourceSubmodule {
    param([string]$Relative, [string]$FullPath, [psobject]$Selection)
    $gitModules = Join-Path $Selection.Root '.gitmodules'
    if (-not (Test-Path -LiteralPath $gitModules -PathType Leaf) -or
        -not (Test-Path -LiteralPath $fullPath -PathType Container)) {
        throw "Source-tree submodule is missing: $relative"
    }
    $declarations = @(git config --file $gitModules --get-regexp '^submodule\..*\.path$')
    if ($LASTEXITCODE -ne 0 -or $relative -cnotin @(
            $declarations | ForEach-Object { ($_ -split '\s+', 2)[1] })) {
        throw "Source-tree submodule is not declared in .gitmodules: $relative"
    }

}

function Get-Fo4HdrSubmoduleSourceFiles {
    param([string]$Relative, [string]$FullPath, [psobject]$Selection)
    if (Test-Path -LiteralPath (Join-Path $fullPath '.git')) {
        $children = @(git -c core.quotePath=false -C $fullPath ls-files --recurse-submodules)
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to enumerate source-tree submodule: $relative"
        }
    } elseif (Test-Path -LiteralPath (Join-Path $Selection.Root '.git')) {
        throw "Initialize the pinned recursive submodule before packaging: $relative"
    } else {
        # Extracted source archives already contain the expanded dependencies.
        $children = @(Get-ChildItem -LiteralPath $fullPath -Force -Recurse -File |
            ForEach-Object { [IO.Path]::GetRelativePath($fullPath, $_.FullName).Replace('\', '/') })
    }
    if ($children.Count -eq 0) {
        throw "Source-tree submodule contains no source files: $relative"
    }
    return $children
}

function Get-Fo4HdrTarListing {
    param([string]$ArchivePath)
    $listInfo = [Diagnostics.ProcessStartInfo]::new()
    $listInfo.FileName = "tar.exe"
    $listInfo.UseShellExecute = $false
    $listInfo.RedirectStandardOutput = $true
    $listInfo.RedirectStandardError = $true
    foreach ($argument in @("-tzf", $archivePath)) {
        $listInfo.ArgumentList.Add($argument)
    }
    $listProcess = [Diagnostics.Process]::new()
    $listProcess.StartInfo = $listInfo
    try {
        if (-not $listProcess.Start()) { throw "Unable to inspect tar archive." }
        $listing = $listProcess.StandardOutput.ReadToEnd()
        $listingError = $listProcess.StandardError.ReadToEnd()
        $listProcess.WaitForExit()
        if ($listProcess.ExitCode -ne 0) {
            throw "Unable to inspect tar archive: $($listingError.Trim())"
        }
    }
    finally {
        $listProcess.Dispose()
    }

    return $listing
}

function Read-Fo4HdrTarEntry {
    param([string]$ArchivePath, [string]$EntryName)
    $readInfo = [Diagnostics.ProcessStartInfo]::new()
    $readInfo.FileName = "tar.exe"
    $readInfo.UseShellExecute = $false
    $readInfo.RedirectStandardOutput = $true
    $readInfo.RedirectStandardError = $true
    foreach ($argument in @("-xOzf", $archivePath, "--", $EntryName)) {
        $readInfo.ArgumentList.Add($argument)
    }
    $readProcess = [Diagnostics.Process]::new()
    $readProcess.StartInfo = $readInfo
    $output = [IO.MemoryStream]::new()
    try {
        if (-not $readProcess.Start()) { throw "Unable to read tar entry." }
        $copy = $readProcess.StandardOutput.BaseStream.CopyToAsync($output)
        $readError = $readProcess.StandardError.ReadToEndAsync()
        $readProcess.WaitForExit()
        [void]$copy.GetAwaiter().GetResult()
        $errorText = $readError.GetAwaiter().GetResult()
        if ($readProcess.ExitCode -ne 0) {
            throw "Unable to read tar entry: $($errorText.Trim())"
        }
        return $output.ToArray()
    }
    finally {
        $output.Dispose()
        $readProcess.Dispose()
    }
}

function Assert-Fo4HdrArchiveEntryName {
    param([string]$Name)
    $unsafe = $name.StartsWith("/", [StringComparison]::Ordinal) -or
        $name -match '^[A-Za-z]:' -or
        $name.Contains([char]0)
    if ($unsafe) {
        throw "Archive contains an unsafe path: $name"
    }

    $segments = @($name.Split('/') | Where-Object { $_ })
    if ($segments | Where-Object { $_ -in @(".", "..") }) {
        throw "Archive contains a traversal path: $name"
    }
}

function Assert-Fo4HdrSpdlogSourceIdentity {
    param([psobject]$Package, [psobject]$Versions, [Text.RegularExpressions.Match]$LockedSpdlog)
    $lockedSpdlogBody = $lockedSpdlog.Groups['body'].Value
    $expectedVersion = $versions.xmake.packages.spdlog.version
    if ($package.version -ne $expectedVersion -or
        $lockedSpdlog.Groups['version'].Value -ne $expectedVersion) {
        throw "The spdlog version differs across the lock and source manifest."
    }
    if ($package.sourceSha256.ToLowerInvariant() -ne
        $versions.xmake.packages.spdlog.sourceSha256.ToLowerInvariant()) {
        throw "The spdlog source hash differs from versions.lock.json."
    }
    $canonicalSource = [ordered]@{
        repository = "https://github.com/gabime/spdlog.git"
        sourceRevision = "v1.16.0"
        recipeRepository = "https://github.com/xmake-io/xmake-repo.git"
    }
    foreach ($field in $canonicalSource.Keys) {
        if ($package.$field -ne $canonicalSource[$field]) {
            throw "The spdlog source or XMake recipe repository is not canonical."
        }
    }
    Assert-Fo4HdrSpdlogRecipeIdentity $package $versions.xmake.packages.spdlog $lockedSpdlogBody

}

function Assert-Fo4HdrImguiSourceIdentity {
    param([psobject]$Imgui, [psobject]$ImguiLock)
    foreach ($field in @("version", "sourceSha256", "recipeCommit", "recipeTreeSha1", "recipeSha256", "licenseSha256")) {
        if ([string]$imgui.$field -ne [string]$imguiLock.$field) {
            throw "The ImGui $field differs across source authorities."
        }
    }
    $invalidIdentity = $imgui.repository -ne "https://github.com/ocornut/imgui.git" -or
        $imgui.sourceRevision -ne $imguiLock.commit -or
        $imgui.sourceArchiveFormat -ne "tar.gz"
    if ($invalidIdentity) {
        throw "The ImGui source identity or archive format is not canonical."
    }
}

function Assert-Fo4HdrSpdlogArchiveContents {
    param([psobject]$Package, [string]$SourcePath, [string]$RecipePath)
    foreach ($licenseFile in @($package.licenseFiles)) {
            $licenseHash = Get-Fo4HdrSourceArchiveEntrySha256 `
                $sourcePath $package.sourceArchiveFormat $licenseFile
            if ($licenseHash -ne
                $package.licenseSha256.ToLowerInvariant()) {
                throw "The spdlog MIT license hash differs from its manifest."
            }
    }

    Assert-Fo4HdrSpdlogRecipeContents $package $recipePath

}

function Assert-Fo4HdrImguiArchiveContents {
    param([psobject]$Imgui, [string]$ImguiSourcePath, [string]$ImguiRecipePath)
    foreach ($licenseFile in @($imgui.licenseFiles)) {
        if ((Get-Fo4HdrSourceArchiveEntrySha256 $imguiSourcePath "tar.gz" $licenseFile) -ne
            $imgui.licenseSha256) {
            throw "The ImGui MIT license hash differs from its manifest."
        }
    }
    Assert-Fo4HdrImguiRecipeContents $imgui $imguiRecipePath

}

function Assert-Fo4HdrSpdlogRecipeContents {
    param([psobject]$Package, [string]$RecipePath)
    $recipeArchive = Open-Fo4HdrValidatedZipArchive $recipePath
    try {
        $recipeText = Read-Fo4HdrZipEntryUtf8Text `
            $recipeArchive `
            "xmake-repo/packages/s/spdlog/xmake.lua"
        foreach ($requiredRecipeText in @(
            'set_license("MIT")',
            $package.version,
            $package.sourceSha256.ToLowerInvariant(),
            'https://github.com/gabime/spdlog'
        )) {
            if ($recipeText -notlike "*$requiredRecipeText*") {
                throw "The archived spdlog recipe is missing: $requiredRecipeText"
            }
        }
    }
    finally {
        $recipeArchive.Dispose()
    }

}

function Assert-Fo4HdrImguiRecipeContents {
    param([psobject]$Imgui, [string]$ImguiRecipePath)
    $imguiRecipe = Open-Fo4HdrValidatedZipArchive $imguiRecipePath
    try {
        $imguiRecipeText = Read-Fo4HdrZipEntryUtf8Text $imguiRecipe "xmake-repo/packages/i/imgui/xmake.lua"
        foreach ($required in @('set_license("MIT")', $imgui.version, $imgui.sourceSha256)) {
            if ($imguiRecipeText -notlike "*$required*") { throw "The archived ImGui recipe is missing: $required" }
        }
    }
    finally { $imguiRecipe.Dispose() }

}

function Assert-Fo4HdrSpdlogRecipeIdentity {
    param([psobject]$Package, [psobject]$LockedPackage, [string]$LockedSpdlogBody)
    if ($lockedSpdlogBody -notmatch
            [regex]::Escape($package.recipeCommit) -or
        $lockedSpdlogBody -notmatch
            [regex]::Escape($package.recipeRepository)) {
        throw "The source manifest recipe identity differs from the XMake lock."
    }
    foreach ($field in @('recipeCommit', 'recipeTreeSha1', 'recipeSha256', 'licenseSha256')) {
        if ($package.$field.ToLowerInvariant() -ne $LockedPackage.$field.ToLowerInvariant()) {
            throw "The spdlog recipe or license identity differs from versions.lock.json."
        }
    }

}

function Resolve-Fo4HdrSourceInventorySetting {
    param([string]$RootPath, [string]$InventoryPath, [bool]$IsCheckout)
    if (-not $InventoryPath -and $isCheckout) {
        $configured = @(git -C $rootPath config --local --get fo4hdr.sourceInventory)
        if ($LASTEXITCODE -notin @(0, 1) -or $configured.Count -gt 1) {
            throw 'Unable to read the checkout source selection.'
        }
        if ($configured.Count) { $InventoryPath = $configured[0] }
    }
    return $InventoryPath
}

function Read-Fo4HdrSourceInventory {
    param([string]$RootPath, [string]$InventoryPath)
    $inventoryPath = if ([IO.Path]::IsPathFullyQualified($InventoryPath)) {
        $InventoryPath
    } else {
        Join-Path $rootPath $InventoryPath
    }
    if (-not (Test-Path -LiteralPath $inventoryPath -PathType Leaf)) {
        throw "Source-tree inventory does not exist: $inventoryPath"
    }
    $entries = @(Get-Content -LiteralPath $inventoryPath)
    return $entries
}

function Assert-Fo4HdrLockedPackageSet {
    param([string]$LockText)
    $lockedPackageKeys = @(
        [regex]::Matches(
            $lockText,
            '(?m)^ {8}\["(?<key>[^"]+)"\]\s*=') |
            ForEach-Object { $_.Groups['key'].Value }
    )
    $expectedLockKeys = @(
        "cmake#f56260b5",
        "ninja#f56260b5",
        "spdlog v1.16.0#09c8c173"
    )
    $expectedLockKeyPrefixes = @("imgui v1.92.6-docking#")
    foreach ($prefix in $expectedLockKeyPrefixes) {
        $match = @($lockedPackageKeys | Where-Object { $_.StartsWith($prefix) })
        if ($match.Count -ne 1) { throw "xmake-requires.lock does not contain exactly one $prefix entry." }
        $expectedLockKeys += $match[0]
    }
    if ($lockedPackageKeys.Count -ne $expectedLockKeys.Count -or
        (Compare-Object $lockedPackageKeys $expectedLockKeys)) {
        throw (
            "The XMake lock package set changed. Review whether each new " +
            "entry is a build tool or a linked dependency."
        )
    }

}

function Add-Fo4HdrSubmoduleSourceFile {
    param([string]$Child, [string]$Relative, [string]$FullPath, [psobject]$Selection)
    $childPath = [IO.Path]::GetFullPath((Join-Path $fullPath $child))
    $expanded = "$relative/$child"
    if (($child.Split('/') | Where-Object { $_ -in @('.git', '.', '..', '') }) -or
        -not $childPath.StartsWith($fullPath + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $childPath -PathType Leaf) -or
        -not $Selection.Seen.Add($expanded)) {
        throw "Invalid or duplicate source-tree dependency file: $expanded"
    }
    $Selection.Files.Add($expanded)
}

function Get-Fo4HdrPackageSourceArtifacts {
    param([psobject]$Package, [string]$ArtifactRoot)
    $sourcePath = Resolve-Fo4HdrChildPath `
        $ArtifactRoot `
        $package.sourceCacheFile
    $recipePath = Resolve-Fo4HdrChildPath `
        $ArtifactRoot `
        $package.recipeCacheFile
    foreach ($path in @($sourcePath, $recipePath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing corresponding-source artifact: $path"
        }
    }
    return [pscustomobject]@{ Source = $sourcePath; Recipe = $recipePath }
}
