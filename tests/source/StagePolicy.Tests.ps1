# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $tempBase ('FO4Isotope-deploy-test-' + [Guid]::NewGuid().ToString('N'))
$project = Join-Path $fixture 'project'
$destination = Join-Path $fixture 'FO4Isotope-mod'
$game = Join-Path $fixture 'game'
$stage = Join-Path $project 'dist/1.10.163/Data'
$deploy = Join-Path $project 'scripts/deploy.ps1'
function Get-DeploymentEnvironment {
    $saved = @{}
    foreach ($name in @('FO4_ROOT', 'FO4_MOD_ROOT', 'FO4HDR_STAGE_ROOT')) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    return $saved
}

$savedEnvironment = Get-DeploymentEnvironment

function Assert-DeployRejected {
    param([string]$Reason)
    try { & $deploy -Runtime '1.10.163' -Confirm:$false }
    catch {
        if ($_.Exception.Message.Contains($Reason, [StringComparison]::Ordinal)) { return }
        throw
    }
    throw "Deployment unexpectedly accepted: $Reason"
}

function Test-DeploymentRejections {
    & $deploy -Runtime '1.10.163' -WhatIf
    $validationComplete = (Test-Path (Join-Path $project 'runtime-checked')) -and
        (Test-Path (Join-Path $project 'metadata-checked'))
    if ((Test-Path $destination) -or -not $validationComplete) {
        throw 'WhatIf must validate the runtime and metadata without creating a deployment.'
    }
    $extraFile = Join-Path $stage 'unexpected.txt'
    [IO.File]::WriteAllText($extraFile, 'extra')
    Assert-DeployRejected 'Staged runtime file set is not exact.'
    Remove-Item -LiteralPath $extraFile
    foreach ($name in @('Default.ini', 'Personal.ini', 'Weather.ini')) {
        $sidecarStage = Join-Path $stage "F4SE/Plugins/FO4Isotope/$name"
        [IO.File]::WriteAllText($sidecarStage, 'must not ship')
        Assert-DeployRejected 'Staged runtime file set is not exact.'
        Remove-Item -LiteralPath $sidecarStage
    }
    $requiredFile = Join-Path $stage $relativeFiles[0]
    Remove-Item -LiteralPath $requiredFile
    Assert-DeployRejected 'Staged runtime file set is not exact.'
    [IO.File]::WriteAllText($requiredFile, "fixture: $($relativeFiles[0])")
    $rejection = Join-Path $project 'reject-metadata'
    [IO.File]::WriteAllText($rejection, 'reject')
    Assert-DeployRejected 'Fixture metadata rejection.'
    Remove-Item -LiteralPath $rejection
    if (Test-Path $destination) { throw 'Rejected staging created a deployment.' }

    $env:FO4_MOD_ROOT = Join-Path $project 'FO4Isotope-mod'
    Assert-DeployRejected 'Deployment destination must not be inside the repository.'
    $env:FO4_MOD_ROOT = Join-Path $game 'FO4Isotope-mod'
    Assert-DeployRejected 'Deployment destination must not be inside FO4_ROOT.'
    $env:FO4_MOD_ROOT = $destination

}

function Test-DeploymentOwnedFiles {
    New-Item -ItemType Directory -Path $destination | Out-Null
    $unrelated = Join-Path $destination 'unrelated.txt'
    $obsolete = Join-Path $destination 'obsolete.txt'
    $personal = Join-Path $destination 'F4SE/Plugins/FO4Isotope/Personal.ini'
    $default = Join-Path $destination 'F4SE/Plugins/FO4Isotope/Default.ini'
    $weather = Join-Path $destination 'F4SE/Plugins/FO4Isotope/Weather.ini'
    New-Item -ItemType Directory -Path (Split-Path $personal) -Force | Out-Null
    [IO.File]::WriteAllText($unrelated, 'keep')
    [IO.File]::WriteAllText($obsolete, 'owned')
    [IO.File]::WriteAllText($personal, 'personal')
    $sidecarBytes = [byte[]](0xEF, 0xBB, 0xBF, 0x23, 0x0D, 0x0A, 0xFF)
    [IO.File]::WriteAllBytes($default, $sidecarBytes)
    [IO.File]::WriteAllBytes($weather, $sidecarBytes)
    $ownedManifest = Join-Path $destination '.fo4isotope-owned-files.json'
    [IO.File]::WriteAllText($ownedManifest,
        '{"files":["obsolete.txt","F4SE/Plugins/FO4Isotope/Personal.ini","F4SE\\Plugins\\FO4Isotope\\Default.ini","F4SE/Plugins/FO4Isotope/Weather.ini"]}')
    & $deploy -Runtime '1.10.163' -Confirm:$false
    Assert-PreservedDeploymentFiles
    Assert-DeployedFiles $ownedManifest
}

function Assert-PreservedDeploymentFiles {
    $unrelated = Join-Path $destination 'unrelated.txt'
    $obsolete = Join-Path $destination 'obsolete.txt'
    $personal = Join-Path $destination 'F4SE/Plugins/FO4Isotope/Personal.ini'
    $default = Join-Path $destination 'F4SE/Plugins/FO4Isotope/Default.ini'
    $weather = Join-Path $destination 'F4SE/Plugins/FO4Isotope/Weather.ini'
    $sidecarBytes = [byte[]](0xEF, 0xBB, 0xBF, 0x23, 0x0D, 0x0A, 0xFF)
    $preserved = [IO.File]::ReadAllText($unrelated) -ceq 'keep' -and
        [IO.File]::ReadAllText($personal) -ceq 'personal'
    foreach ($sidecar in @($default, $weather)) {
        $preserved = $preserved -and (Test-Path -LiteralPath $sidecar) -and
            ([Convert]::ToHexString($sidecarBytes) -ceq
                [Convert]::ToHexString([IO.File]::ReadAllBytes($sidecar)))
    }
    if ((Test-Path $obsolete) -or -not $preserved) {
        throw 'Deployment must preserve unrelated files and all retired sidecar INI bytes.'
    }
}

function Assert-DeployedFiles {
    param([string]$OwnedManifest)
    foreach ($relative in $relativeFiles) {
        if ([IO.File]::ReadAllText((Join-Path $destination $relative)) -cne "fixture: $relative") {
            throw "Deployment did not copy the expected bytes: $relative"
        }
    }
    $recordedFiles = @((Get-Content -LiteralPath $ownedManifest -Raw | ConvertFrom-Json).files |
        ForEach-Object { $_.Replace('\', '/') })
    if (Compare-Object $relativeFiles $recordedFiles) { throw 'Owned-file manifest differs from deployment.' }
}

function Test-BuildStageRetirement {
    $default = Join-Path $stage 'F4SE/Plugins/FO4Isotope/Default.ini'
    $userDefault = Join-Path $fixture 'user/Default.ini'
    New-Item -ItemType Directory -Path (Split-Path $userDefault) -Force | Out-Null
    [IO.File]::WriteAllText($default, 'retired generated default')
    [IO.File]::WriteAllText($userDefault, 'keep user data')
    function xmake {
        if ($args[0] -eq 'install' -and (Test-Path -LiteralPath $default)) {
            throw 'Incremental staging retained the retired default.'
        }
        $global:LASTEXITCODE = 0
    }
    & (Join-Path $project 'scripts/build.ps1') -Runtime '1.10.163' -Mode debug
    if ((Test-Path -LiteralPath $default) -or
        [IO.File]::ReadAllText($userDefault) -cne 'keep user data') {
        throw 'Build cleanup must affect only the retired generated stage file.'
    }
}

function Remove-DeploymentFixture {
    param([hashtable]$SavedEnvironment, [string]$Fixture, [string]$TempBase)
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolved -Leaf) -notlike 'FO4Isotope-deploy-test-*') {
        throw 'Refusing to remove an unexpected test fixture path.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

function Copy-DeploymentScripts {
    New-Item -ItemType Directory -Path (Join-Path $project 'scripts/internal'), $game -Force | Out-Null
    foreach ($script in @('build.ps1', 'deploy.ps1', 'internal/Project.Common.ps1', 'internal/Project.Process.ps1',
        'internal/Project.Toolchain.ps1', 'internal/Project.Runtime.ps1', 'internal/Project.Runtime.Binary.ps1',
        'internal/Project.Runtime.Profiles.ps1', 'internal/Project.Files.ps1',
        'internal/Project.Source.ps1')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "scripts/$script") -Destination (Join-Path $project "scripts/$script")
    }
}

function Write-RuntimeValidationStub {
    # Game/PE/toolchain checks are stubbed; cleanup, stage validation and copying use the real scripts.
    [IO.File]::WriteAllText((Join-Path $project 'scripts/validate-runtime.ps1'), @"
param([string]`$Runtime)
if (`$Runtime -cne '1.10.163') { throw 'Unexpected runtime validation request.' }
[IO.File]::WriteAllText((Join-Path `$PSScriptRoot '../runtime-checked'), 'checked')
"@)
}

function Write-MetadataValidationStub {
    [IO.File]::WriteAllText((Join-Path $project 'scripts/verify-plugin-metadata.ps1'), @"
param([string]`$Runtime, [string]`$Mode, [switch]`$StageOnly)
if (`$Runtime -cne '1.10.163') { throw 'Unexpected metadata runtime.' }
if (-not `$StageOnly -and `$Mode -cne 'debug') { throw 'Expected stage-only or fixture build metadata verification.' }
if (Test-Path (Join-Path `$PSScriptRoot '../reject-metadata')) { throw 'Fixture metadata rejection.' }
[IO.File]::WriteAllText((Join-Path `$PSScriptRoot '../metadata-checked'), 'checked')
"@)
}

function Write-BuildConfigurationStub {
    [IO.File]::WriteAllText((Join-Path $project 'scripts/configure.ps1'), @'
param([string]$Runtime, [string]$Mode)
if ($Runtime -cne '1.10.163' -or $Mode -cne 'debug') { throw 'Unexpected fixture build configuration.' }
'@)
}

function Initialize-DeploymentStage {
    foreach ($relative in $relativeFiles) {
        $file = Join-Path $stage $relative
        New-Item -ItemType Directory -Path (Split-Path $file) -Force | Out-Null
        [IO.File]::WriteAllText($file, "fixture: $relative")
    }
}

$relativeFiles = @(
    'F4SE/Plugins/FO4Isotope.dll',
    'F4SE/Plugins/FO4Isotope.pdb',
    'F4SE/Plugins/FO4Isotope.ini',
    'F4SE/Plugins/FO4Isotope/runtime-1.10.163.json'
)

try {
    Copy-DeploymentScripts
    Write-RuntimeValidationStub
    Write-MetadataValidationStub
    Write-BuildConfigurationStub
    Initialize-DeploymentStage
    $env:FO4_ROOT = $game
    $env:FO4_MOD_ROOT = $destination
    $env:FO4HDR_STAGE_ROOT = Join-Path $fixture 'unused-stage'

    Test-BuildStageRetirement
    Test-DeploymentRejections
    Test-DeploymentOwnedFiles
}
finally {
    Remove-DeploymentFixture $savedEnvironment $fixture $tempBase
}
Write-Host 'Deployment staging, validation, WhatIf, and owned-file behavior tests passed.'
