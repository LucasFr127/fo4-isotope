# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Actual,

        [Parameter(Mandatory = $true)]
        [object]$Expected,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Actual -cne $Expected) {
        throw "$Message Expected '$Expected'; observed '$Actual'."
    }
}

function Assert-Throws {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    try {
        & $Action
    }
    catch {
        return
    }

    throw $Message
}

function Write-SyntheticPe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $bytes = [byte[]]::new(0x400)
    $stream = [IO.MemoryStream]::new($bytes, $true)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0x5A4D)
        $stream.Position = 0x3C
        $writer.Write([uint32]0x80)
        $stream.Position = 0x80
        $writer.Write([uint32]0x00004550)
        $writer.Write([uint16]0x8664)
        $writer.Write([uint16]2)
        $writer.Write([uint32]0)
        $writer.Write([uint32]0)
        $writer.Write([uint32]0)
        $writer.Write([uint16]0xF0)
        $writer.Write([uint16]0x0022)
        $writer.Write([uint16]0x020B)
        $stream.Position = 0x80 + 24 + 56
        $writer.Write([uint32]0x5000)

        $stream.Position = 0x188
        $writer.Write(
            [Text.Encoding]::ASCII.GetBytes(".text`0`0`0")
        )
        $writer.Write([uint32]0x1000)
        $writer.Write([uint32]0x1000)
        $writer.Write([uint32]0x200)
        $writer.Write([uint32]0x200)
        $stream.Position = 0x188 + 36
        $writer.Write([uint32]0x60000020)

        $stream.Position = 0x1B0
        $writer.Write(
            [Text.Encoding]::ASCII.GetBytes(".data`0`0`0")
        )
        $writer.Write([uint32]0x1000)
        $writer.Write([uint32]0x3000)
        $writer.Write([uint32]0x200)
        $writer.Write([uint32]0x400)
        $stream.Position = 0x1B0 + 36
        $writer.Write([uint32]0xC0000040L)
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }

    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-AddressLibraryV0 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Entries,

        [Nullable[uint64]]$DeclaredCount
    )

    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None
    )
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $count = if ($null -ne $DeclaredCount) {
            [uint64]$DeclaredCount
        } else {
            [uint64]$Entries.Count
        }
        $writer.Write($count)
        foreach ($entry in $Entries) {
            $writer.Write([uint64]$entry.Id)
            $writer.Write([uint64]$entry.Offset)
        }
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function New-RequiredRelocation {
    param(
        [Parameter(Mandatory = $true)]
        [uint64]$Id,

        [Parameter(Mandatory = $true)]
        [string]$Kind,

        [uint64]$Rva
    )

    return [pscustomobject]@{
        provider = "provider-$Id"
        id = $Id
        kind = $Kind
        rva = $Rva
    }
}

$tempRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    ("Fallout4HDR-address-v0-tests-" + [Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    $exePath = Join-Path $tempRoot "synthetic.exe"
    $databasePath = Join-Path $tempRoot "address.bin"
    Write-SyntheticPe $exePath

    $validEntries = @(
        [pscustomobject]@{ Id = 1; Offset = 0x1010 },
        [pscustomobject]@{ Id = 2; Offset = 0x3010 },
        [pscustomobject]@{ Id = 3; Offset = 0x3020 }
    )
    $validRequired = @(
        New-RequiredRelocation 1 "function" 0x1010
        New-RequiredRelocation 2 "pointer-storage" 0x3010
        New-RequiredRelocation 3 "direct-object" 0x3020
    )
    Write-AddressLibraryV0 $databasePath $validEntries
    $valid = Assert-Fo4HdrAddressLibraryV0 `
        $databasePath `
        $exePath `
        $validRequired
    Assert-Equal $valid.Count ([uint64]3) "Valid V0 count mismatch."
    Assert-Equal `
        $valid.RequiredCount `
        ([uint64]3) `
        "Valid required count mismatch."

    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 `
            $databasePath `
            $exePath `
            @((New-RequiredRelocation 1 "function" 0x1020)) |
            Out-Null
    } "An exact RVA mismatch was not rejected."

    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 `
            $databasePath `
            $exePath `
            @((New-RequiredRelocation 2 "function" 0x3010)) |
            Out-Null
    } "A function mapped to data was not rejected."
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 `
            $databasePath `
            $exePath `
            @((New-RequiredRelocation 1 "pointer-storage" 0x1010)) |
            Out-Null
    } "Data mapped to code was not rejected."
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 `
            $databasePath `
            $exePath `
            @((New-RequiredRelocation 4 "function" 0x1010)) |
            Out-Null
    } "A required ID above the database maximum was not rejected."

    $outsideImage = @(
        [pscustomobject]@{ Id = 1; Offset = 0x6000 }
    )
    Write-AddressLibraryV0 $databasePath $outsideImage
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 `
            $databasePath `
            $exePath `
            @((New-RequiredRelocation 1 "function" 0x6000)) |
            Out-Null
    } "An out-of-image required RVA was not rejected."

    Write-AddressLibraryV0 $databasePath @()
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "A zero-entry database was not rejected."

    $missingInterior = @(
        [pscustomobject]@{ Id = 1; Offset = 0x1010 },
        [pscustomobject]@{ Id = 3; Offset = 0x3020 }
    )
    Write-AddressLibraryV0 $databasePath $missingInterior
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 `
            $databasePath `
            $exePath `
            @((New-RequiredRelocation 2 "pointer-storage" 0x3010)) |
            Out-Null
    } "A missing interior ID was not rejected."

    Write-AddressLibraryV0 `
        $databasePath `
        $validEntries `
        -DeclaredCount 4
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "A truncated database was not rejected."
    Write-AddressLibraryV0 `
        $databasePath `
        $validEntries `
        -DeclaredCount 2
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "A database with a bad count was not rejected."

    Write-AddressLibraryV0 `
        $databasePath `
        @() `
        -DeclaredCount ([uint64]::MaxValue)
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "An overflowing database count was not rejected."

    $duplicate = @(
        [pscustomobject]@{ Id = 1; Offset = 0x1010 },
        [pscustomobject]@{ Id = 1; Offset = 0x1020 }
    )
    Write-AddressLibraryV0 $databasePath $duplicate
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "Duplicate IDs were not rejected."

    $unsorted = @(
        [pscustomobject]@{ Id = 2; Offset = 0x3010 },
        [pscustomobject]@{ Id = 1; Offset = 0x1010 }
    )
    Write-AddressLibraryV0 $databasePath $unsorted
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "Unsorted IDs were not rejected."

    $zeroOffset = @(
        [pscustomobject]@{ Id = 1; Offset = 0 }
    )
    Write-AddressLibraryV0 $databasePath $zeroOffset
    Assert-Throws {
        Assert-Fo4HdrAddressLibraryV0 $databasePath $exePath @() |
            Out-Null
    } "A zero offset was not rejected."


}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}

Write-Host "Address Library V0 regression tests passed."
