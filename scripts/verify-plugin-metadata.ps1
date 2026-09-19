# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet("1.10.163")]
    [string]$Runtime = "1.10.163",

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$StageOnly
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$lock = Get-Content (
    Join-Path $repoRoot "versions.lock.json"
) -Raw | ConvertFrom-Json
$entry = $lock.runtimes.$Runtime
$stageRoot = Join-Path $repoRoot "dist"
$builtDll =
    Join-Path $repoRoot "build\windows\x64\$Mode\Fallout4HDR.dll"
$pluginRoot =
    Join-Path $stageRoot "$Runtime\Data\F4SE\Plugins"
$stagedDll = Join-Path $pluginRoot "Fallout4HDR.dll"
$stagedManifest = Join-Path `
    $pluginRoot `
    "Fallout4HDR\runtime-$Runtime.json"

foreach ($path in @(
    $stagedDll,
    (Join-Path $pluginRoot "Fallout4HDR.pdb"),
    (Join-Path $pluginRoot "Fallout4HDR.ini"),
    $stagedManifest
)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Expected built/staged plugin file is missing: $path"
    }
}

if (-not $StageOnly) {
    if (-not (Test-Path -LiteralPath $builtDll -PathType Leaf)) {
        throw "Expected built plugin file is missing: $builtDll"
    }
    if ((Get-Sha256 $builtDll) -ne (Get-Sha256 $stagedDll)) {
        throw "The staged plugin DLL differs from the selected build output."
    }
}
if ((Get-Fo4HdrCanonicalTextSha256 $stagedManifest) -ne
    $entry.runtimeManifestCanonicalSha256) {
    throw "The staged runtime manifest differs from versions.lock.json."
}

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

    $dataVersion =
        [Runtime.InteropServices.Marshal]::ReadInt32($version, 0x000)
    $pluginName =
        [Runtime.InteropServices.Marshal]::PtrToStringAnsi(
            [IntPtr]::Add($version, 0x008))
    $addressIndependence =
        [Runtime.InteropServices.Marshal]::ReadInt32($version, 0x208)
    $structureIndependence =
        [Runtime.InteropServices.Marshal]::ReadInt32($version, 0x20C)

    if ($dataVersion -ne 1 -or $pluginName -ne "Fallout4HDR") {
        throw "The exported F4SE plugin identity is invalid."
    }
    if ($addressIndependence -ne 4 -or
        $structureIndependence -ne 4) {
        throw "The exported Address Library/layout flags are invalid."
    }

    $parts = $Runtime.Split('.') | ForEach-Object { [uint32]$_ }
    $expectedPacked =
        (($parts[0] -band 0xFF) -shl 24) -bor
        (($parts[1] -band 0xFF) -shl 16) -bor
        (($parts[2] -band 0xFFF) -shl 4)
    for ($index = 0; $index -lt 16; ++$index) {
        $actual = [uint32][Runtime.InteropServices.Marshal]::ReadInt32(
            $version,
            0x210 + ($index * 4))
        $expected = if ($index -eq 0) { $expectedPacked } else { 0 }
        if ($actual -ne $expected) {
            throw (
                "PluginVersion compatible runtimes are not exactly " +
                "RUNTIME_1_10_163 followed by zero terminators."
            )
        }
    }

    $minimumF4se =
        [uint32][Runtime.InteropServices.Marshal]::ReadInt32(
            $version,
            0x250)
    $expectedF4se =
        (0 -shl 24) -bor (6 -shl 16) -bor (23 -shl 4)
    if ($minimumF4se -ne $expectedF4se) {
        throw "PluginVersion does not require exactly F4SE 0.6.23."
    }
}
finally {
    [void][Fo4Hdr.NativeMethods]::FreeLibrary($module)
}

Write-Host (
    "Verified plugin staging and exact 1.10.163 metadata" +
    $(if ($StageOnly) { "." } else { " for $Mode." })
)
