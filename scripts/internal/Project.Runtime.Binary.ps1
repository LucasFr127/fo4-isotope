# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

Set-StrictMode -Version Latest

function Invoke-Fo4HdrBinaryReader {
    param([string]$Path, [long]$MinimumLength, [string]$TruncatedMessage, [scriptblock]$Read)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -lt $MinimumLength) { throw $TruncatedMessage }
        $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::UTF8, $true)
        try { return & $Read $reader }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Assert-Fo4HdrPeSignature {
    param([uint32]$Actual, [uint32]$Expected, [string]$Message)
    if ($Actual -ne $Expected) { throw $Message }
}

function Read-Fo4HdrPeHeader {
    param([IO.BinaryReader]$Reader, [string]$Path)
    $stream = $Reader.BaseStream
    Assert-Fo4HdrPeSignature ($reader.ReadUInt16()) 0x5A4D "PE image has no DOS MZ signature: $Path"

    $stream.Position = 0x3C
    $peOffset = $reader.ReadUInt32()
    if ([uint64]$peOffset + 24 -gt [uint64]$stream.Length) {
        throw "PE header lies outside the image: $Path"
    }

    $stream.Position = $peOffset
    Assert-Fo4HdrPeSignature ($reader.ReadUInt32()) 0x00004550 "PE image has no PE signature: $Path"
    Assert-Fo4HdrPeSignature ($reader.ReadUInt16()) 0x8664 "PE image is not AMD64: $Path"

    $sectionCount = $reader.ReadUInt16()
    if ($sectionCount -eq 0) {
        throw "PE image has no sections: $Path"
    }

    return Read-Fo4HdrPeOptionalHeader $reader $peOffset $sectionCount $Path

}

function Read-Fo4HdrPeOptionalHeader {
    param([IO.BinaryReader]$Reader, [uint32]$PeOffset, [uint16]$SectionCount, [string]$Path)
    $stream = $Reader.BaseStream
    $stream.Position = [uint64]$peOffset + 20
    $optionalHeaderSize = $reader.ReadUInt16()
    $stream.Position = [uint64]$peOffset + 24
    if ($optionalHeaderSize -lt 60 -or
        [uint64]$stream.Position + $optionalHeaderSize -gt
            [uint64]$stream.Length) {
        throw "PE optional header lies outside the image: $Path"
    }
    Assert-Fo4HdrPeSignature ($reader.ReadUInt16()) 0x020B "PE image is not PE32+: $Path"

    $stream.Position = [uint64]$peOffset + 24 + 56
    $sizeOfImage = [uint64]$reader.ReadUInt32()
    if ($sizeOfImage -eq 0) {
        throw "PE image has a zero SizeOfImage: $Path"
    }

    $sectionTable = [uint64]$peOffset + 24 +
        [uint64]$optionalHeaderSize
    $sectionBytes = [uint64]$sectionCount * 40
    if ($sectionTable + $sectionBytes -gt
        [uint64]$stream.Length) {
        throw "PE section table lies outside the image: $Path"
    }


    return [pscustomobject]@{SectionCount=$SectionCount; SectionTable=$sectionTable; SizeOfImage=$sizeOfImage}
}

function Read-Fo4HdrPeSectionName {
    param([IO.BinaryReader]$Reader)
    $nameBytes = $reader.ReadBytes(8)
    $nameLength = [Array]::IndexOf[byte]($nameBytes, 0)
    if ($nameLength -lt 0) {
        $nameLength = $nameBytes.Length
    }
    return [Text.Encoding]::ASCII.GetString(
        $nameBytes,
        0,
        $nameLength
    )
}

function Read-Fo4HdrPeSection {
    param([IO.BinaryReader]$Reader, $Header, [int]$Index)
    $stream = $Reader.BaseStream
    $sectionTable = $Header.SectionTable
    $sizeOfImage = $Header.SizeOfImage
    $stream.Position = $sectionTable + [uint64]($index * 40)
    $name = Read-Fo4HdrPeSectionName $reader
    $virtualSize = [uint64]$reader.ReadUInt32()
    $virtualAddress = [uint64]$reader.ReadUInt32()
    $rawSize = [uint64]$reader.ReadUInt32()
    $span = [Math]::Max($virtualSize, $rawSize)

    $stream.Position = $sectionTable +
        [uint64]($index * 40) + 36
    $characteristics = $reader.ReadUInt32()
    if ($span -eq 0) {
        return $null
    }
    if ($virtualAddress -gt [uint64]::MaxValue - $span) {
        throw "PE section address range overflows: $name"
    }
    if ($virtualAddress + $span -gt $sizeOfImage) {
        throw "PE section lies outside SizeOfImage: $name"
    }

    return [pscustomobject]@{
        Name = $name
        StartRva = $virtualAddress
        EndRvaExclusive = $virtualAddress + $span
        Characteristics = $characteristics
    }
}

function Get-Fo4HdrPeSections {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "PE image does not exist: $Path"
    }

    return Invoke-Fo4HdrBinaryReader $Path 64 "PE image is too short: $Path" {
        param($Reader)
        $header = Read-Fo4HdrPeHeader $reader $Path
        $sectionCount = $header.SectionCount
        $sections = [Collections.Generic.List[object]]::new()
        for ($index = 0; $index -lt $sectionCount; ++$index) {
            $section = Read-Fo4HdrPeSection $reader $header $index
            if ($null -ne $section) { $sections.Add($section) }
        }

        if ($sections.Count -eq 0) {
            throw "PE image has no addressable sections: $Path"
        }
        return $sections.ToArray()
    }
}

function ConvertTo-Fo4HdrRequiredRelocation {
    param($Relocation)
    try {
        $id = [uint64]$relocation.id
    }
    catch {
        throw "Required relocation has an invalid unsigned ID."
    }
    if ($id -eq 0) {
        throw "Required relocation ID must be nonzero."
    }

    $provider = [string]$relocation.provider
    if ([string]::IsNullOrWhiteSpace($provider)) {
        throw "Required relocation has no provider name."
    }

    $kind = [string]$relocation.kind
    if ($kind -notin @(
        "function",
        "pointer-storage",
        "direct-object"
    )) {
        throw "Required relocation '$provider' has invalid kind '$kind'."
    }

    try {
        $expectedOffset = [uint64]$relocation.rva
    }
    catch {
        throw "Required relocation '$provider' has an invalid RVA."
    }
    if ($expectedOffset -eq 0) {
        throw "Required relocation '$provider' must have a nonzero RVA."
    }
    return [pscustomobject]@{Id=$id; Provider=$provider; Kind=$kind; ExpectedOffset=$expectedOffset}
}

function New-Fo4HdrRequiredRelocations {
    param([AllowEmptyCollection()][object[]]$RequiredRelocations)
    $requiredById =
        [Collections.Generic.Dictionary[uint64, object]]::new()
    foreach ($relocation in $RequiredRelocations) {
        $validated = ConvertTo-Fo4HdrRequiredRelocation $relocation
        $id = $validated.Id
        $provider = $validated.Provider
        $kind = $validated.Kind
        $expectedOffset = $validated.ExpectedOffset
        if ($requiredById.ContainsKey($id)) {
            throw "Required relocation ID is duplicated: $id"
        }

        $requiredById.Add($id, [pscustomobject]@{
            Provider = $provider
            Kind = $kind
            ExpectedOffset = $expectedOffset
            Found = $false
            Offset = [uint64]0
            Section = $null
        })
    }

    return ,$requiredById
}

function Read-Fo4HdrAddressLibraryCount {
    param([IO.BinaryReader]$Reader)
    $stream = $Reader.BaseStream
    $count = $reader.ReadUInt64()
    if ($count -eq 0) {
        throw "Address Library V0 contains no entries."
    }
    $maximumCount = ([uint64][long]::MaxValue - 8) / 16
    if ($count -gt $maximumCount) {
        throw "Address Library V0 count overflows the file format."
    }

    $expectedLength = [uint64]8 + ($count * [uint64]16)
    if ([uint64]$stream.Length -ne $expectedLength) {
        throw (
            "Address Library V0 length mismatch. Expected " +
            "$expectedLength bytes for $count entries; found " +
            "$($stream.Length)."
        )
    }

    return $count
}

function Test-Fo4HdrCodeSection {
    param($Section)
    return (($Section.Characteristics -band 0x20) -ne 0) -and
        (($Section.Characteristics -band 0x20000000) -ne 0)
}

function Test-Fo4HdrDataSection {
    param($Section)
    return (($Section.Characteristics -band 0x20000000) -eq 0) -and
        (($Section.Characteristics -band 0x40000000) -ne 0) -and
        (($Section.Characteristics -band 0xC0) -ne 0)
}

function Get-Fo4HdrRequiredRelocationSection {
    param($Required, [uint64]$Id, [uint64]$Offset, [object[]]$Sections)
    $matches = @(
        $sections |
            Where-Object {
                $offset -ge $_.StartRva -and
                $offset -lt $_.EndRvaExclusive
            }
    )
    if ($matches.Count -ne 1) {
        throw (
            "Required relocation '$($required.Provider)' " +
            "ID $id offset 0x$($offset.ToString('X')) does " +
            "not map to exactly one PE section."
        )
    }

    return $matches[0]
}

function Assert-Fo4HdrRequiredRelocation {
    param($Required, [uint64]$Id, [uint64]$Offset, [object[]]$Sections)
    if ($offset -ne $required.ExpectedOffset) {
        throw (
            "Required relocation '$($required.Provider)' " +
            "ID $id RVA mismatch. Expected " +
            "0x$($required.ExpectedOffset.ToString('X')); " +
            "found 0x$($offset.ToString('X'))."
        )
    }
    $section = Get-Fo4HdrRequiredRelocationSection $Required $Id $Offset $Sections
    $isCode = Test-Fo4HdrCodeSection $section
    $isData = Test-Fo4HdrDataSection $section
    if ($required.Kind -eq "function" -and -not $isCode) {
        throw (
            "Required function '$($required.Provider)' ID " +
            "$id resolves to non-code section " +
            "'$($section.Name)'."
        )
    }
    if ($required.Kind -ne "function" -and -not $isData) {
        throw (
            "Required data '$($required.Provider)' ID $id " +
            "resolves to implausible data section " +
            "'$($section.Name)'."
        )
    }

    $required.Found = $true
    $required.Offset = $offset
    $required.Section = $section.Name
}

function Assert-Fo4HdrRequiredRelocationsFound {
    param($RequiredById)
    foreach ($pair in $requiredById.GetEnumerator()) {
        if (-not $pair.Value.Found) {
            throw (
                "Required relocation '$($pair.Value.Provider)' ID " +
                "$($pair.Key) is absent from Address Library V0."
            )
        }
    }

}

function Read-Fo4HdrAddressLibraryEntries {
    param([IO.BinaryReader]$Reader, [uint64]$Count, $RequiredById, [object[]]$Sections)
    $hasPrevious = $false
    $previousId = [uint64]0
    $minimumId = [uint64]0
    $maximumId = [uint64]0
    for ($index = [uint64]0; $index -lt $count; ++$index) {
        $id = $reader.ReadUInt64()
        $offset = $reader.ReadUInt64()
        if ($hasPrevious -and $id -le $previousId) {
            throw (
                "Address Library V0 IDs are duplicate or unsorted " +
                "at entry ${index}: $id follows $previousId."
            )
        }
        if ($offset -eq 0) {
            throw "Address Library V0 entry $id has a zero offset."
        }

        if (-not $hasPrevious) {
            $minimumId = $id
        }
        $hasPrevious = $true
        $previousId = $id
        $maximumId = $id

        if ($requiredById.ContainsKey($id)) {
            Assert-Fo4HdrRequiredRelocation $requiredById[$id] $id $offset $sections
        }
    }

    Assert-Fo4HdrRequiredRelocationsFound $requiredById

    return [pscustomobject]@{
        Count = $count
        MinimumId = $minimumId
        MaximumId = $maximumId
        RequiredCount = [uint64]$requiredById.Count
    }
}

function Assert-Fo4HdrAddressLibraryV0 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$RequiredRelocations
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Address Library database does not exist: $Path"
    }

    $sections = @(Get-Fo4HdrPeSections $ExecutablePath)
    $requiredById = New-Fo4HdrRequiredRelocations $RequiredRelocations

    return Invoke-Fo4HdrBinaryReader $Path 8 "Address Library V0 header is truncated: $Path" {
        param($Reader)
        $count = Read-Fo4HdrAddressLibraryCount $reader

        return Read-Fo4HdrAddressLibraryEntries $reader $count $requiredById $sections
    }
}

