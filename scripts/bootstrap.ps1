# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")
. (Join-Path $PSScriptRoot "internal/CommonLibIntegrity.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$toolchain = Get-ActiveToolchainInfo
Assert-X64Toolchain $toolchain

$lockPath = Join-Path $repoRoot "versions.lock.json"
$lock = Get-Content $lockPath -Raw | ConvertFrom-Json
$pinned = $lock.toolchain.observed

$toolchainOptions = @{ Lock = $lock }
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "visualStudioProductLine" `
    $pinned.visualStudio.productLine `
    $toolchain.VisualStudioProductLine
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "visualStudioInstallation" `
    $pinned.visualStudio.installationVersion `
    $toolchain.VisualStudioInstallationVersion
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "msvcCompiler" `
    $pinned.msvc.compilerVersion `
    $toolchain.MsvcCompilerVersion
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "msvcTools" `
    $pinned.msvc.toolsVersion `
    $toolchain.MsvcToolsVersion
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "msvcHostArchitecture" `
    $pinned.msvc.hostArchitecture `
    $toolchain.HostArchitecture
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "msvcTargetArchitecture" `
    $pinned.msvc.targetArchitecture `
    $toolchain.TargetArchitecture
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "windowsSdk" `
    $pinned.windowsSdk.version `
    $toolchain.WindowsSdkVersion
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "xmake" `
    $pinned.xmake `
    $toolchain.XMakeVersion
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "powerShell" `
    $pinned.powerShell `
    $toolchain.PowerShellVersion
Assert-Fo4HdrToolchainValue @toolchainOptions `
    "git" `
    $pinned.git `
    $toolchain.GitVersion

if ($lock.commonlib.commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "CommonLibF4 is not pinned in versions.lock.json."
}
if ($lock.commonlib.commonlibSharedCommit -notmatch
    '^[0-9a-fA-F]{40}$') {
    throw "commonlib-shared is not pinned in versions.lock.json."
}
if ($lock.commonlib.dearModdingUiApiCommit -notmatch
    '^[0-9a-fA-F]{40}$') {
    throw "DearModdingUI API is not pinned in versions.lock.json."
}

Assert-Fo4HdrCommonLibRootGitlink `
    -RepoRoot $repoRoot `
    -Lock $lock

$commonlibInitialized = Test-Fo4HdrGitSubmoduleInitialized `
    -ParentRoot $repoRoot `
    -RelativePath 'lib/commonlibf4'
if (-not $commonlibInitialized) {
    throw @"
CommonLibF4 is uninitialized. The approved repository declaration and indexed
gitlink match versions.lock.json. In a fresh checkout, run:

git submodule update --init --recursive -- lib/commonlibf4

Then rerun scripts/bootstrap.ps1. Do not use scripts/add-commonlib.ps1 for an
existing checkout, and do not use submodule update to repair an initialized
mismatch.
"@
}

Assert-Fo4HdrCommonLibGitIntegrity `
    -RepoRoot $repoRoot `
    -Lock $lock

Write-Host "Toolchain policy: $(Get-Fo4HdrToolchainPolicy)"
$toolchain | Format-List
Write-Host "Bootstrap validation passed."
