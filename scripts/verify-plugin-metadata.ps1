# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.240")]
    [string]$Runtime = "1.10.163",

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$StageOnly,

    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

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
    if ($addressIndependence -ne 4 -or $structureIndependence -ne 0) {
        throw "The exported Address Library/layout flags are invalid."
    }

    Assert-Fo4HdrPluginVersionRequirements $Version $Runtime $F4seVersion
}

function Assert-Fo4HdrPluginVersionRequirements {
    param([IntPtr]$Version, [string]$Runtime, [string]$F4seVersion)
    $expectedPacked = @(
        (ConvertTo-Fo4HdrPackedPluginVersion '1.10.163'),
        (ConvertTo-Fo4HdrPackedPluginVersion '1.10.984'),
        (ConvertTo-Fo4HdrPackedPluginVersion '1.11.240')
    )
    for ($index = 0; $index -lt 16; ++$index) {
        $actual = [uint32][Runtime.InteropServices.Marshal]::ReadInt32(
            $version,
            0x210 + ($index * 4))
        $expected = if ($index -lt $expectedPacked.Count) { $expectedPacked[$index] } else { 0 }
        if ($actual -ne $expected) {
            throw (
                "PluginVersion compatible runtimes are not exactly " +
                "OG 1.10.163, NG 1.10.984 and AE 1.11.240 followed by zero terminators."
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
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x20C, 0)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x210, $Fixture.Packed)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x214, 0x010A3D80)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x218, 0x010B0F00)
        [Runtime.InteropServices.Marshal]::WriteInt32($version, 0x250, $Fixture.Minimum)
        $arguments = @{
            Version = $version
            Runtime = $Fixture.Runtime
            F4seVersion = $Lock.runtimes.'1.10.163'.f4seVersion
        }
        Assert-Fo4HdrPluginVersion @arguments
        $invalidFields = @(
            @{ Offset = 0x000; Value = 2 },
            @{ Offset = 0x008; Value = 0 },
            @{ Offset = 0x208; Value = 0 },
            @{ Offset = 0x208; Value = 6 },
            @{ Offset = 0x208; Value = 6 - $Fixture.Flags },
            @{ Offset = 0x20C; Value = 4 },
            @{ Offset = 0x20C; Value = 6 },
            @{ Offset = 0x20C; Value = 6 - $Fixture.Flags },
            @{ Offset = 0x210; Value = 0 },
            @{ Offset = 0x210; Value = $Fixture.Packed + 16 },
            @{ Offset = 0x214; Value = $Fixture.Packed },
            @{ Offset = 0x214; Value = 0x010B0F00 },
            @{ Offset = 0x214; Value = 0x010B0DD0 },
            @{ Offset = 0x218; Value = $Fixture.Packed },
            @{ Offset = 0x218; Value = 0 },
            @{ Offset = 0x21C; Value = $Fixture.Packed },
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
        @{ Runtime = '1.10.984'; Packed = 0x010A0A30; Flags = 4; Minimum = 0x00060170 },
        @{ Runtime = '1.11.240'; Packed = 0x010A0A30; Flags = 4; Minimum = 0x00060170 }
    )) {
        Assert-Fo4HdrPluginMetadataFixture $fixture $Lock
    }
    Write-Host "Shared OG/NG/AE plugin metadata self-tests passed."
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
        (Join-Path $pluginRoot "FO4Isotope.ini")
    )) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Expected built/staged plugin file is missing: $path"
        }
    }

    if (-not $StageOnly) { Assert-Fo4HdrBuiltPlugin }
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
    Join-Path $stageRoot "universal\Data\F4SE\Plugins"
$stagedDll = Join-Path $pluginRoot "FO4Isotope.dll"

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

    Assert-Fo4HdrPluginVersion -Version $version -Runtime $Runtime -F4seVersion $lock.runtimes.'1.10.163'.f4seVersion
}
finally {
    [void][Fo4Hdr.NativeMethods]::FreeLibrary($module)
}

Write-Host (
    "Verified shared OG/NG/AE plugin metadata and staged DLL" +
    $(if ($StageOnly) { "." } else { " for $Mode." })
)
