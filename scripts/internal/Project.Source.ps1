# SPDX-License-Identifier: GPL-3.0-or-later
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
