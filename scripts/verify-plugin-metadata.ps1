# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime = "1.10.163",

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$StageOnly,

    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

function ConvertTo-Fo4HdrPackedPluginVersion {
    param([string]$Version)
    $parts = $Version.Split('.') | ForEach-Object { [uint32]$_ }
    return (($parts[0] -band 0xFF) -shl 24) -bor
        (($parts[1] -band 0xFF) -shl 16) -bor
        (($parts[2] -band 0xFFF) -shl 4)
}

function Assert-Fo4HdrPluginVersion {
    param([IntPtr]$Version, [string]$Runtime, [string]$F4seVersion)
    $dataVersion =
        [Runtime.InteropServices.Marshal]::ReadInt32($version, 0x000)
    $pluginName =
        [Runtime.InteropServices.Marshal]::PtrToStringAnsi(
            [IntPtr]::Add($version, 0x008), 256).Split([char]0)[0]
    $addressIndependence =
        [Runtime.InteropServices.Marshal]::ReadInt32($version, 0x208)
    $structureIndependence =
        [Runtime.InteropServices.Marshal]::ReadInt32($version, 0x20C)

    if ($dataVersion -ne 1 -or $pluginName -ne "FO4Isotope") {
        throw "The exported F4SE plugin identity is invalid."
    }
    $expectedFlags = if ($Runtime -eq "1.10.984") { 2 } else { 4 }
    if ($addressIndependence -ne $expectedFlags -or
        $structureIndependence -ne $expectedFlags) {
        throw "The exported Address Library/layout flags are invalid."
    }

    Assert-Fo4HdrPluginVersionRequirements $Version $Runtime $F4seVersion
}

function Assert-Fo4HdrPluginVersionRequirements {
    param([IntPtr]$Version, [string]$Runtime, [string]$F4seVersion)
    $expectedPacked = ConvertTo-Fo4HdrPackedPluginVersion $Runtime
    for ($index = 0; $index -lt 16; ++$index) {
        $actual = [uint32][Runtime.InteropServices.Marshal]::ReadInt32(
            $version,
            0x210 + ($index * 4))
        $expected = if ($index -eq 0) { $expectedPacked } else { 0 }
        if ($actual -ne $expected) {
            throw (
                "PluginVersion compatible runtimes are not exactly " +
                "$Runtime followed by zero terminators."
            )
        }
    }

    $minimumF4se =
        [uint32][Runtime.InteropServices.Marshal]::ReadInt32(
            $version,
            0x250)
    $expectedF4se = ConvertTo-Fo4HdrPackedPluginVersion $F4seVersion
    if ($minimumF4se -ne $expectedF4se) {
        throw "PluginVersion does not require exactly F4SE $F4seVersion."
    }
}

function Assert-Fo4HdrCorruptPluginMetadataRejected {
    param($Arguments, [int]$Offset, [int]$Value)
    $version = $Arguments.Version
    $original = [Runtime.InteropServices.Marshal]::ReadInt32($version, $Offset)
    [Runtime.InteropServices.Marshal]::WriteInt32($version, $Offset, $Value)
    $rejected = $false
    try { Assert-Fo4HdrPluginVersion @Arguments } catch { $rejected = $true }
    [Runtime.InteropServices.Marshal]::WriteInt32($version, $Offset, $original)
    if (-not $rejected) {
        throw "Invalid metadata accepted for $($Arguments.Runtime) at $Offset."
    }
}

function Assert-Fo4HdrPluginMetadataFixture {
    param($Fixture, $Lock)
    # The buffer and offsets follow the pinned F4SE::PluginVersionData ABI.
    $version = [Runtime.InteropServices.Marshal]::AllocHGlobal(0x45C)
    try {
        $bytes = [byte[]]::new(0x45C)
        [Text.Encoding]::ASCII.GetBytes("FO4Isotope").CopyTo($bytes, 0x008)
        [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $version, $bytes.Length)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x000, 1)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x208, $Fixture.Flags)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x20C, $Fixture.Flags)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x210, $Fixture.Packed)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x250, $Fixture.Minimum)
        $arguments = @{
            Version = $version
            Runtime = $Fixture.Runtime
            F4seVersion = $Lock.runtimes.($Fixture.Runtime).f4seVersion
        }
        Assert-Fo4HdrPluginVersion @arguments
        $invalidFields = @(
            @{ Offset = 0x000; Value = 2 },
            @{ Offset = 0x008; Value = 0 },
            @{ Offset = 0x208; Value = 0 },
            @{ Offset = 0x208; Value = 6 },
            @{ Offset = 0x208; Value = 6 - $Fixture.Flags },
            @{ Offset = 0x20C; Value = 0 },
            @{ Offset = 0x20C; Value = 6 },
            @{ Offset = 0x20C; Value = 6 - $Fixture.Flags },
            @{ Offset = 0x210; Value = 0 },
            @{ Offset = 0x210; Value = $Fixture.Packed + 16 },
            @{ Offset = 0x214; Value = $Fixture.Packed },
            @{ Offset = 0x24C; Value = $Fixture.Packed },
            @{ Offset = 0x250; Value = 0 },
            @{ Offset = 0x250; Value = $Fixture.Minimum - 16 },
            @{ Offset = 0x250; Value = $Fixture.Minimum + 16 }
        )
        foreach ($field in $invalidFields) {
            Assert-Fo4HdrCorruptPluginMetadataRejected $arguments $field.Offset $field.Value
        }
    }
    finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($version)
    }
}

function Invoke-Fo4HdrPluginMetadataSelfTest {
    param($Lock)
    foreach ($fixture in @(
        @{ Runtime = '1.10.163'; Packed = 0x010A0A30; Flags = 4; Minimum = 0x00060170 },
        @{ Runtime = '1.10.984'; Packed = 0x010A3D80; Flags = 2; Minimum = 0x00070020 },
        @{ Runtime = '1.11.221'; Packed = 0x010B0DD0; Flags = 4; Minimum = 0x00070080 }
    )) {
        Assert-Fo4HdrPluginMetadataFixture $fixture $Lock
    }
    Assert-Fo4HdrStagedManifestFixtures
    Write-Host "Plugin metadata self-tests passed for OG, NG and AE."
}

function Assert-Fo4HdrStagedManifestHash {
    param([string]$StagedHash, [string]$SourceHash, $Entry)
    if ($StagedHash -cne $SourceHash) {
        throw "The staged runtime manifest differs from the selected source manifest."
    }
    if ($Entry.status -in @("runtime-pinned-unvalidated", "capture-complete", "release-approved") -and
        $StagedHash -cne $Entry.runtimeManifestCanonicalSha256) {
        throw "The staged runtime manifest differs from versions.lock.json."
    }
}

function Assert-Fo4HdrStagedManifestFixtures {
    foreach ($case in @(
        @{ Status = 'planned'; Source = 'a'; Staged = 'a'; Locked = $null; Accept = $true },
        @{ Status = 'planned'; Source = 'a'; Staged = 'b'; Locked = $null; Accept = $false },
        @{ Status = 'runtime-pinned-unvalidated'; Source = 'a'; Staged = 'a'; Locked = 'a'; Accept = $true },
        @{ Status = 'runtime-pinned-unvalidated'; Source = 'a'; Staged = 'a'; Locked = 'b'; Accept = $false },
        @{ Status = 'runtime-pinned-unvalidated'; Source = 'a'; Staged = 'b'; Locked = 'b'; Accept = $false }
    )) {
        $entry = [pscustomobject]@{ status = $case.Status }
        if ($case.Locked) {
            $entry | Add-Member -NotePropertyName runtimeManifestCanonicalSha256 -NotePropertyValue $case.Locked
        }
        $accepted = $true
        try {
            Assert-Fo4HdrStagedManifestHash $case.Staged $case.Source $entry
        } catch { $accepted = $false }
        if ($accepted -ne $case.Accept) {
            throw "Staged manifest hash regression for $($case.Status)."
        }
    }
}

function Assert-Fo4HdrBuiltPlugin {
    if (-not (Test-Path -LiteralPath $builtDll -PathType Leaf)) {
        throw "Expected built plugin file is missing: $builtDll"
    }
    if ((Get-Sha256 $builtDll) -ne (Get-Sha256 $stagedDll)) {
        throw "The staged plugin DLL differs from the selected build output."
    }
}

function Assert-Fo4HdrPluginStaging {
    foreach ($path in @(
        $stagedDll,
        (Join-Path $pluginRoot "FO4Isotope.pdb"),
        (Join-Path $pluginRoot "FO4Isotope.ini"),
        $stagedManifest
    )) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Expected built/staged plugin file is missing: $path"
        }
    }

    if (-not $StageOnly) { Assert-Fo4HdrBuiltPlugin }

    Assert-Fo4HdrStagedManifestHash `
        -StagedHash (Get-Fo4HdrCanonicalTextSha256 $stagedManifest) `
        -SourceHash (Get-Fo4HdrCanonicalTextSha256 (Join-Path $repoRoot "runtime/$Runtime/manifest.json")) `
        -Entry $entry
}

$repoRoot = Get-Fo4HdrRepoRoot
$lock = Get-Content (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
if ($SelfTest) {
    Invoke-Fo4HdrPluginMetadataSelfTest -Lock $lock
    return
}
$entry = $lock.runtimes.$Runtime
$stageRoot = Join-Path $repoRoot "dist"
$builtDll =
    Join-Path $repoRoot "build\windows\x64\$Mode\FO4Isotope.dll"
$pluginRoot =
    Join-Path $stageRoot "$Runtime\Data\F4SE\Plugins"
$stagedDll = Join-Path $pluginRoot "FO4Isotope.dll"
$stagedManifest = Join-Path `
    $pluginRoot `
    "FO4Isotope\runtime-$Runtime.json"

Assert-Fo4HdrPluginStaging

if (-not ("Fo4Hdr.NativeMethods" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace Fo4Hdr
{
    public static class NativeMethods
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr LoadLibraryExW(
            string fileName,
            IntPtr file,
            uint flags);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        public static extern IntPtr GetProcAddress(
            IntPtr module,
            string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool FreeLibrary(IntPtr module);
    }
}
"@
}

$dontResolveDllReferences = 0x00000001
$dllToInspect = if ($StageOnly) { $stagedDll } else { $builtDll }
$module = [Fo4Hdr.NativeMethods]::LoadLibraryExW(
    $dllToInspect,
    [IntPtr]::Zero,
    $dontResolveDllReferences)
if ($module -eq [IntPtr]::Zero) {
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "Unable to map the plugin DLL for metadata inspection: $errorCode"
}

try {
    $query = [Fo4Hdr.NativeMethods]::GetProcAddress(
        $module,
        "F4SEPlugin_Query")
    $load = [Fo4Hdr.NativeMethods]::GetProcAddress(
        $module,
        "F4SEPlugin_Load")
    $version = [Fo4Hdr.NativeMethods]::GetProcAddress(
        $module,
        "F4SEPlugin_Version")
    if ($query -eq [IntPtr]::Zero -or
        $load -eq [IntPtr]::Zero -or
        $version -eq [IntPtr]::Zero) {
        throw "The plugin does not export the required F4SE entry points."
    }

    Assert-Fo4HdrPluginVersion -Version $version -Runtime $Runtime -F4seVersion $entry.f4seVersion
}
finally {
    [void][Fo4Hdr.NativeMethods]::FreeLibrary($module)
}

Write-Host (
    "Verified plugin staging and exact $Runtime metadata" +
    $(if ($StageOnly) { "." } else { " for $Mode." })
)
