# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.4.6

[CmdletBinding()]
param(
    [ValidateSet('debug', 'releasedbg')][string]$Mode = 'releasedbg',
    [ValidateSet('unit', 'shader', 'all')][string]$Scope = 'unit',
    [ValidateRange(1, 2147483)][int]$TimeoutSeconds = 120,
    [switch]$AllowShaderSkip,
    [switch]$Gt7Reference,
    [switch]$SourceChecks
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))
. ([IO.Path]::Combine($PSScriptRoot, 'internal/Project.Process.ps1'))
$build = [IO.Path]::Combine($root, "build/windows/x64/$Mode")

function Invoke-SelectedTest {
    param([string]$Executable, [string[]]$Arguments, [bool]$Shader = $false)

    if (-not [IO.File]::Exists($Executable)) { throw "Missing test executable: $Executable" }
    $result = Invoke-Fo4HdrTestProcess -FilePath $Executable -Arguments $Arguments `
        -WorkingDirectory $root -TimeoutSeconds $TimeoutSeconds `
        -AllowShaderSkip:($Shader -and $AllowShaderSkip)
    if ($result.stdout) { Write-Output $result.stdout }
    if ($result.stderr) { [Console]::Error.Write($result.stderr) }
    if ($result.category -notin @('passed', 'skipped')) {
        throw "Test $($result.category), exit=$($result.exitCode), cleanup=$($result.cleanup): $Executable. $($result.detail)"
    }
    Write-Host "$($result.category): $Executable"
}

function Invoke-UnitTests {
    $plugin = [IO.Path]::Combine($build, 'Fallout4HDR.dll')
    if (-not [IO.File]::Exists($plugin)) { throw "Missing plugin fixture: $plugin" }
    foreach ($name in @('fo4hdr-tests', 'fo4hdr-tests-executable-mismatch')) {
        Invoke-SelectedTest ([IO.Path]::Combine($build, "$name.exe")) @($plugin)
    }
}

function Invoke-ShaderTests {
    $inputName = if ($Gt7Reference) { 'Gt7Evaluation.dxbc' } else { 'TransferEvaluation.dxbc' }
    $shader = [IO.Path]::Combine($build, $inputName)
    if (-not [IO.File]::Exists($shader)) { throw "Missing shader input: $shader" }
    $arguments = if ($Gt7Reference) { @('--gt7-reference', $shader) } else { @($shader) }
    Invoke-SelectedTest ([IO.Path]::Combine($build, 'fo4hdr-shader-tests.exe')) $arguments $true
}

function Invoke-SourceChecks {
    $powerShell = if ($env:FO4HDR_PWSH) { $env:FO4HDR_PWSH }
        else { (Get-Command pwsh -CommandType Application -ErrorAction Stop).Source }
    Invoke-SelectedTest $powerShell @('-NoProfile', '-NonInteractive', '-File',
        [IO.Path]::Combine($root, 'tests/source/SourceTree.Tests.ps1'))
    Invoke-SelectedTest $powerShell @('-NoProfile', '-NonInteractive', '-File',
        [IO.Path]::Combine($root, 'tests/source/PackageSource.Tests.ps1'))
    Invoke-SelectedTest $powerShell @('-NoProfile', '-NonInteractive', '-File',
        [IO.Path]::Combine($root, 'tests/source/SourceArchive.Tests.ps1'))
    Invoke-SelectedTest $powerShell @('-NoProfile', '-NonInteractive', '-File',
        [IO.Path]::Combine($root, 'tests/source/TestProcess.Tests.ps1'))
    Invoke-SelectedTest $powerShell @('-NoProfile', '-NonInteractive', '-File',
        [IO.Path]::Combine($root, 'tests/runtime/MonitorNativeSession.Tests.ps1'))
    Invoke-SelectedTest $powerShell @('-NoProfile', '-NonInteractive', '-File',
        [IO.Path]::Combine($root, 'tests/source/RuntimePinning.Tests.ps1'),
        '-FixturePath', [IO.Path]::Combine($build, 'fo4hdr-tests.exe'),
        '-MismatchFixturePath', [IO.Path]::Combine($build, 'fo4hdr-tests-executable-mismatch.exe'))
}

function Invoke-RequestedTests {
    if ($Gt7Reference -and $Scope -eq 'unit') { throw 'Gt7Reference requires shader scope.' }
    if ($Scope -in @('unit', 'all')) { Invoke-UnitTests }
    if ($Scope -in @('shader', 'all')) { Invoke-ShaderTests }
    if ($SourceChecks) { Invoke-SourceChecks }
}

Invoke-RequestedTests
