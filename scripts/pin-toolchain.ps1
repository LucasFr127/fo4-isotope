# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$toolchain = Get-ActiveToolchainInfo
Assert-X64Toolchain $toolchain

$lockPath = Join-Path $repoRoot "versions.lock.json"
$lock = Get-Content $lockPath -Raw | ConvertFrom-Json

$lock.toolchain.observed.visualStudio.productLine =
    $toolchain.VisualStudioProductLine
$lock.toolchain.observed.visualStudio.installationVersion =
    $toolchain.VisualStudioInstallationVersion
$lock.toolchain.observed.msvc.compilerVersion =
    $toolchain.MsvcCompilerVersion
$lock.toolchain.observed.msvc.toolsVersion =
    $toolchain.MsvcToolsVersion
$lock.toolchain.observed.msvc.hostArchitecture =
    $toolchain.HostArchitecture
$lock.toolchain.observed.msvc.targetArchitecture =
    $toolchain.TargetArchitecture
$lock.toolchain.observed.windowsSdk.version =
    $toolchain.WindowsSdkVersion
$lock.toolchain.observed.xmake =
    $toolchain.XMakeVersion
$lock.toolchain.observed.powerShell =
    $toolchain.PowerShellVersion
$lock.toolchain.observed.git =
    $toolchain.GitVersion

Write-Fo4HdrJsonFile `
    -Value $lock `
    -Path $lockPath `
    -SchemaPath (Join-Path $repoRoot "schemas\versions-lock.schema.json")

& (Join-Path $PSScriptRoot "validate-manifests.ps1")

$toolchain | Format-List
Write-Host "Pinned the active x64 toolchain in versions.lock.json."
