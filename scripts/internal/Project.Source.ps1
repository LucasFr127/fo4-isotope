# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#requires -Version 7.4.6

Add-Type -AssemblyName System.IO.Compression

function Assert-Fo4HdrArchiveSegment {
    param([string]$Segment)

    if ($Segment -match '^$|^\.\.?$|[ .]$') { throw "Unsafe archive segment: $Segment" }
    if ($Segment.Split('.')[0] -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
        throw "Reserved Windows archive segment: $Segment"
    }
}

function Assert-Fo4HdrArchiveEntryName {
    param([string]$Name)

    if ($Name -match '^$|^/|//|[\\:\x00-\x1f<>"|?*]') { throw "Unsafe archive entry: $Name" }
    $path = $Name.TrimEnd('/')
    foreach ($segment in $path.Split('/')) { Assert-Fo4HdrArchiveSegment $segment }
    return $path
}

function Open-Fo4HdrValidatedZipArchive {
    param([string]$Path)

    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            $name = Assert-Fo4HdrArchiveEntryName $entry.FullName
            if (-not $names.Add($name)) { throw "Duplicate archive entry: $($entry.FullName)" }
        }
        return $archive
    }
    catch {
        $archive.Dispose()
        throw
    }
}

function Add-Fo4HdrLockedPackageArchives {
    param([string]$Root, [Collections.Generic.SortedDictionary[string, byte[]]]$Sources,
        [string]$SpdlogArchive, [string]$ImguiArchive)

    $manifest = Get-Content -LiteralPath ([IO.Path]::Combine($Root,
        'third_party/xmake-source-manifest.json')) -Raw | ConvertFrom-Json
    $lock = Get-Content -LiteralPath ([IO.Path]::Combine($Root,
        'versions.lock.json')) -Raw | ConvertFrom-Json
    $requires = [IO.Path]::Combine($Root, 'xmake-requires.lock')
    if ((Get-FileHash -LiteralPath $requires -Algorithm SHA256).Hash -ine $lock.xmake.requiresLockSha256) {
        throw 'Package requirements differ from the exact lock.'
    }
    if (-not $manifest.complete -or @($manifest.packages).Count -ne 2) {
        throw 'Expected the complete ImGui/spdlog source manifest.'
    }
    foreach ($package in $manifest.packages) {
        Add-Fo4HdrLockedPackageArchive $package $lock $Sources $SpdlogArchive $ImguiArchive
    }
}

function Add-Fo4HdrLockedPackageArchive {
    param($Package, $Lock, [Collections.Generic.SortedDictionary[string, byte[]]]$Sources,
        [string]$SpdlogArchive, [string]$ImguiArchive)

    if ($Package.name -cnotin @('spdlog', 'imgui')) { throw 'Unexpected linked package.' }
    $locked = $Lock.xmake.packages.($Package.name)
    foreach ($field in @('version', 'sourceSha256', 'recipeCommit', 'recipeTreeSha1', 'recipeSha256', 'licenseSha256')) {
        if ($Package.$field -cne $locked.$field) { throw "Package identity differs from lock: $field" }
    }
    $source = if ($Package.name -ceq 'spdlog') { $SpdlogArchive } else { $ImguiArchive }
    Add-Fo4HdrSourceArchiveBytes $Package.sourceCacheFile $source $Package.sourceSha256 $Sources
    if (-not $env:FO4HDR_SOURCE_CACHE) { throw 'FO4HDR_SOURCE_CACHE is required for exact package recipes.' }
    $recipe = [IO.Path]::Combine($env:FO4HDR_SOURCE_CACHE, $Package.recipeCacheFile)
    Add-Fo4HdrSourceArchiveBytes $Package.recipeCacheFile $recipe $Package.recipeSha256 $Sources
}

function Add-Fo4HdrSourceArchiveBytes {
    param([string]$Name, [string]$Path, [string]$Expected,
        [Collections.Generic.SortedDictionary[string, byte[]]]$Sources)

    Assert-Fo4HdrArchiveEntryName $Name | Out-Null
    if ($Name.Contains('/')) { throw 'Package source artifact name must be a filename.' }
    if (-not [IO.File]::Exists($Path)) { throw 'Package source artifact is missing.' }
    if ([IO.File]::GetAttributes($Path) -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Package source artifact is redirected.'
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    if ($actual -cne $Expected) { throw "Exact package source artifact differs: $Name" }
    $Sources.Add('third_party/xmake-package-sources/' + $Name, $bytes)
}
