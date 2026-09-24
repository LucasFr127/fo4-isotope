# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Steam", "GOG")]
    [string]$Platform,

    [string]$OutputPath,

    [switch]$PassThru,

    [switch]$NoWrite
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

if (-not $env:FO4_ROOT) {
    throw "Set FO4_ROOT to the game installation directory before scanning."
}

$definition = Get-Fo4HdrRuntimeDefinition $Runtime
if ($Platform -notin $definition.AllowedPlatforms) {
    throw (
        "Runtime $Runtime is not approved for platform $Platform. " +
        "Currently allowed: " +
        ($definition.AllowedPlatforms -join ", ")
    )
}

$exe = Join-Path $env:FO4_ROOT "Fallout4.exe"
$loader = Join-Path $env:FO4_ROOT "f4se_loader.exe"
$runtimeDll = Join-Path $env:FO4_ROOT $definition.RuntimeDll
$address = Get-Fo4AddressLibraryPath `
    $definition.AddressLibraryFile

foreach ($path in @($exe, $loader, $runtimeDll, $address)) {
    if (-not (Test-Path $path)) {
        throw "Missing required runtime file: $path"
    }
}

$fileVersion =
    [Diagnostics.FileVersionInfo]::GetVersionInfo(
        $exe
    ).FileVersion

Assert-Fo4HdrExactRuntimeFileVersion `
    -Runtime $Runtime `
    -FileVersion $fileVersion

$observation = [ordered]@{
    schemaVersion = 1
    observedAtUtc = [DateTime]::UtcNow.ToString("o")
    runtime = $Runtime
    fileVersion = $fileVersion
    platform = $Platform.ToLowerInvariant()
    fo4Root = [IO.Path]::GetFullPath($env:FO4_ROOT)
    fallout4Exe = [ordered]@{
        path = [IO.Path]::GetFullPath($exe)
        sha256 = Get-Sha256 $exe
    }
    f4seLoader = [ordered]@{
        path = [IO.Path]::GetFullPath($loader)
        sha256 = Get-Sha256 $loader
    }
    f4seRuntime = [ordered]@{
        version = $definition.F4SEVersion
        path = [IO.Path]::GetFullPath($runtimeDll)
        sha256 = Get-Sha256 $runtimeDll
    }
    addressLibrary = [ordered]@{
        file = $definition.AddressLibraryFile
        path = [IO.Path]::GetFullPath($address)
        sha256 = Get-Sha256 $address
    }
}

if ($NoWrite -and $OutputPath) {
    throw "Do not combine -NoWrite with -OutputPath."
}

if (-not $NoWrite) {
    if (-not $OutputPath) {
        $repoRoot = Get-Fo4HdrRepoRoot
        $OutputPath =
            Join-Path $repoRoot "build\observed-runtime-$Runtime.json"
    }

    $parent = Split-Path -Parent $OutputPath
    if ($parent) {
        New-Item -ItemType Directory -Force $parent |
            Out-Null
    }

    Write-Fo4HdrJsonFile `
        -Value $observation `
        -Path $OutputPath `
        -SchemaPath (Join-Path (Get-Fo4HdrRepoRoot) "schemas\observed-runtime.schema.json") `
        -Depth 16

    Write-Host "Observed runtime written to: $OutputPath"
}

Write-Host "Fallout4.exe: $($observation.fallout4Exe.path)"
Write-Host "SHA-256:      $($observation.fallout4Exe.sha256)"

if ($PassThru) {
    return [pscustomobject]$observation
}
