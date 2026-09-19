# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot 'scripts/internal/Project.Common.ps1')
$lock = Get-Content (Join-Path $repoRoot 'versions.lock.json') -Raw | ConvertFrom-Json
$pinned = $lock.toolchain.observed
$savedPolicy = $env:FO4HDR_TOOLCHAIN_POLICY
$savedToolset = $env:VCToolsVersion

function Assert-ToolchainResult {
    param([string]$Key, [string]$Expected, [string]$Actual, [bool]$Accepted)
    $message = ''
    try { Assert-Fo4HdrToolchainValue -Key $Key -Expected $Expected -Actual $Actual -Lock $lock }
    catch { $message = $_.Exception.Message }
    if (($message.Length -eq 0) -ne $Accepted) {
        throw "Unexpected $env:FO4HDR_TOOLCHAIN_POLICY result for $Key '$Actual': $message"
    }
}

function Assert-ConfiguredMsvcResult {
    param([string]$Tools, [string]$Sdk, [bool]$Accepted, [int]$ExitCode = 0)
    function xmake {
        $global:LASTEXITCODE = $ExitCode
        [pscustomobject]@{ msvcTools = $Tools; windowsSdk = $Sdk } | ConvertTo-Json
    }
    $message = ''
    try { Assert-Fo4HdrConfiguredMsvc -Lock $lock }
    catch { $message = $_.Exception.Message }
    if (($message.Length -eq 0) -ne $Accepted) {
        throw "Unexpected configured MSVC result under $env:FO4HDR_TOOLCHAIN_POLICY`: $message"
    }
}

try {
    $env:FO4HDR_TOOLCHAIN_POLICY = ''
    if ((Get-Fo4HdrToolchainPolicy) -cne 'pinned') { throw 'The default policy must be pinned.' }
    foreach ($policy in @('pinned')) {
        $env:FO4HDR_TOOLCHAIN_POLICY = $policy
        foreach ($case in @(
            @{ Key = 'visualStudioInstallation'; Expected = $pinned.visualStudio.installationVersion },
            @{ Key = 'msvcCompiler'; Expected = $pinned.msvc.compilerVersion },
            @{ Key = 'msvcTools'; Expected = $pinned.msvc.toolsVersion },
            @{ Key = 'powerShell'; Expected = $pinned.powerShell }
        )) {
            $version = [version]$case.Expected
            $newer = '{0}.{1}.{2}' -f $version.Major, $version.Minor, ($version.Build + 1)
            if ($version.Revision -ge 0) { $newer += '.' + $version.Revision }
            $older = '{0}.{1}.{2}' -f $version.Major, $version.Minor, ($version.Build - 1)
            Assert-ToolchainResult $case.Key $case.Expected $case.Expected $true
            Assert-ToolchainResult $case.Key $case.Expected $newer $false
            Assert-ToolchainResult $case.Key $case.Expected $older $false
            Assert-ToolchainResult $case.Key $case.Expected '99.0.0' $false
            Assert-ToolchainResult $case.Key $case.Expected '0.0.0' $false
            Assert-ToolchainResult $case.Key $case.Expected ($newer + '-preview') $false
        }
        Assert-ToolchainResult 'powerShell' $pinned.powerShell '7.60.0' $false
        Assert-ToolchainResult 'powerShell' $pinned.powerShell '7.7.0' $false
        Assert-ToolchainResult 'msvcCompiler' $pinned.msvc.compilerVersion '19.52.0' $false
        Assert-ToolchainResult 'msvcTools' $pinned.msvc.toolsVersion '14.52.0' $false
        Assert-ToolchainResult 'git' $pinned.git '2.55.0.windows.99' $false
        Assert-ToolchainResult 'git' $pinned.git 'unknown' $false
        foreach ($case in @(
            @{ Key = 'windowsSdk'; Expected = $pinned.windowsSdk.version },
            @{ Key = 'xmake'; Expected = $pinned.xmake },
            @{ Key = 'visualStudioProductLine'; Expected = $pinned.visualStudio.productLine }
        )) {
            Assert-ToolchainResult $case.Key $case.Expected $case.Expected $true
            Assert-ToolchainResult $case.Key $case.Expected '99.0.0' $false
        }
        Assert-ToolchainResult 'powerShell' 'FILL_LOCAL' $pinned.powerShell $false
        $toolsVersion = [version]$pinned.msvc.toolsVersion
        $env:VCToolsVersion = '{0}.{1}.{2}' -f $toolsVersion.Major, $toolsVersion.Minor, ($toolsVersion.Build + 1)
        $expectedToolset = $pinned.msvc.toolsVersion
        if ((Get-Fo4HdrMsvcToolset -Lock $lock) -cne $expectedToolset) {
            throw "XMake toolset selection did not honor $policy."
        }
        Assert-ConfiguredMsvcResult $pinned.msvc.toolsVersion $pinned.windowsSdk.version $true
        Assert-ConfiguredMsvcResult $env:VCToolsVersion $pinned.windowsSdk.version $false
        Assert-ConfiguredMsvcResult '14.52.0' $pinned.windowsSdk.version $false
        Assert-ConfiguredMsvcResult $pinned.msvc.toolsVersion '10.0.99999.0' $false
        Assert-ConfiguredMsvcResult $pinned.msvc.toolsVersion $pinned.windowsSdk.version $false 23
        foreach ($architecture in @('HostArchitecture', 'TargetArchitecture')) {
            $toolchain = [pscustomobject]@{
                HostArchitecture = 'x64'; TargetArchitecture = 'x64'
                WindowsSdkVersion = $pinned.windowsSdk.version
            }
            Assert-X64Toolchain $toolchain
            $toolchain.$architecture = 'x86'
            $rejected = $false
            try { Assert-X64Toolchain $toolchain }
            catch { $rejected = $true }
            if (-not $rejected) { throw "$architecture accepted x86 under $policy." }
        }
    }
    $env:FO4HDR_TOOLCHAIN_POLICY = 'unknown'
    Assert-ToolchainResult 'powerShell' $pinned.powerShell $pinned.powerShell $false
}
finally {
    $env:FO4HDR_TOOLCHAIN_POLICY = $savedPolicy
    $env:VCToolsVersion = $savedToolset
}
Write-Host 'Pinned toolchain acceptance, version rejection, and x64 checks passed.'
