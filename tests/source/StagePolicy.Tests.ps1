# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $tempBase ('Fallout4HDR-deploy-test-' + [Guid]::NewGuid().ToString('N'))
$project = Join-Path $fixture 'project'
$destination = Join-Path $fixture 'Fallout4HDR-mod'
$game = Join-Path $fixture 'game'
$stage = Join-Path $project 'dist/1.10.163/Data'
$deploy = Join-Path $project 'scripts/deploy.ps1'
$savedEnvironment = @{}
foreach ($name in @('FO4_ROOT', 'FO4_MOD_ROOT', 'FO4HDR_STAGE_ROOT')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

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
    $personalStage = Join-Path $stage 'F4SE/Plugins/Fallout4HDR/Personal.ini'
    [IO.File]::WriteAllText($personalStage, 'must not ship')
    Assert-DeployRejected 'Staged runtime file set is not exact.'
    Remove-Item -LiteralPath $personalStage
    $requiredFile = Join-Path $stage $relativeFiles[0]
    Remove-Item -LiteralPath $requiredFile
    Assert-DeployRejected 'Staged runtime file set is not exact.'
    [IO.File]::WriteAllText($requiredFile, "fixture: $($relativeFiles[0])")
    $rejection = Join-Path $project 'reject-metadata'
    [IO.File]::WriteAllText($rejection, 'reject')
    Assert-DeployRejected 'Fixture metadata rejection.'
    Remove-Item -LiteralPath $rejection
    if (Test-Path $destination) { throw 'Rejected staging created a deployment.' }

    $env:FO4_MOD_ROOT = Join-Path $project 'Fallout4HDR-mod'
    Assert-DeployRejected 'Deployment destination must not be inside the repository.'
    $env:FO4_MOD_ROOT = Join-Path $game 'Fallout4HDR-mod'
    Assert-DeployRejected 'Deployment destination must not be inside FO4_ROOT.'
    $env:FO4_MOD_ROOT = $destination

}

function Test-DeploymentOwnedFiles {
    New-Item -ItemType Directory -Path $destination | Out-Null
    $unrelated = Join-Path $destination 'unrelated.txt'
    $obsolete = Join-Path $destination 'obsolete.txt'
    $personal = Join-Path $destination 'F4SE/Plugins/Fallout4HDR/Personal.ini'
    New-Item -ItemType Directory -Path (Split-Path $personal) -Force | Out-Null
    [IO.File]::WriteAllText($unrelated, 'keep')
    [IO.File]::WriteAllText($obsolete, 'owned')
    [IO.File]::WriteAllText($personal, 'personal')
    $ownedManifest = Join-Path $destination '.fallout4hdr-owned-files.json'
    [IO.File]::WriteAllText($ownedManifest,
        '{"files":["obsolete.txt","F4SE/Plugins/Fallout4HDR/Personal.ini"]}')
    & $deploy -Runtime '1.10.163' -Confirm:$false
    $preserved = [IO.File]::ReadAllText($unrelated) -ceq 'keep' -and
        [IO.File]::ReadAllText($personal) -ceq 'personal'
    if ((Test-Path $obsolete) -or -not $preserved) {
        throw 'Deployment must preserve unrelated files and Personal.ini.'
    }
    foreach ($relative in $relativeFiles) {
        if ([IO.File]::ReadAllText((Join-Path $destination $relative)) -cne "fixture: $relative") {
            throw "Deployment did not copy the expected bytes: $relative"
        }
    }
    $recordedFiles = @((Get-Content -LiteralPath $ownedManifest -Raw | ConvertFrom-Json).files |
        ForEach-Object { $_.Replace('\', '/') })
    if (Compare-Object $relativeFiles $recordedFiles) { throw 'Owned-file manifest differs from deployment.' }
}

function Remove-DeploymentFixture {
    param([hashtable]$SavedEnvironment, [string]$Fixture, [string]$TempBase)
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolved -Leaf) -notlike 'Fallout4HDR-deploy-test-*') {
        throw 'Refusing to remove an unexpected test fixture path.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

try {
    New-Item -ItemType Directory -Path (Join-Path $project 'scripts/internal'), $game -Force | Out-Null
    foreach ($script in @('deploy.ps1', 'internal/Project.Common.ps1', 'internal/Project.Process.ps1',
        'internal/Project.Toolchain.ps1', 'internal/Project.Runtime.ps1', 'internal/Project.Runtime.Binary.ps1',
        'internal/Project.Runtime.Profiles.ps1', 'internal/Project.Files.ps1',
        'internal/Project.Source.ps1')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "scripts/$script") -Destination (Join-Path $project "scripts/$script")
    }
    # Only the installed-game and PE checks are stubbed; file validation and copying use the real deployment script.
    [IO.File]::WriteAllText((Join-Path $project 'scripts/validate-runtime.ps1'), @"
param([string]`$Runtime)
if (`$Runtime -cne '1.10.163') { throw 'Unexpected runtime validation request.' }
[IO.File]::WriteAllText((Join-Path `$PSScriptRoot '../runtime-checked'), 'checked')
"@)
    [IO.File]::WriteAllText((Join-Path $project 'scripts/verify-plugin-metadata.ps1'), @"
param([string]`$Runtime, [switch]`$StageOnly)
if (`$Runtime -cne '1.10.163') { throw 'Unexpected metadata runtime.' }
if (-not `$StageOnly) { throw 'Expected stage-only metadata verification.' }
if (Test-Path (Join-Path `$PSScriptRoot '../reject-metadata')) { throw 'Fixture metadata rejection.' }
[IO.File]::WriteAllText((Join-Path `$PSScriptRoot '../metadata-checked'), 'checked')
"@)
    $relativeFiles = @(
        'F4SE/Plugins/Fallout4HDR.dll',
        'F4SE/Plugins/Fallout4HDR.pdb',
        'F4SE/Plugins/Fallout4HDR.ini',
        'F4SE/Plugins/Fallout4HDR/Default.ini',
        'F4SE/Plugins/Fallout4HDR/runtime-1.10.163.json'
    )
    foreach ($relative in $relativeFiles) {
        $file = Join-Path $stage $relative
        New-Item -ItemType Directory -Path (Split-Path $file) -Force | Out-Null
        [IO.File]::WriteAllText($file, "fixture: $relative")
    }
    $env:FO4_ROOT = $game
    $env:FO4_MOD_ROOT = $destination
    $env:FO4HDR_STAGE_ROOT = Join-Path $fixture 'unused-stage'

    Test-DeploymentRejections
    Test-DeploymentOwnedFiles
}
finally {
    Remove-DeploymentFixture $savedEnvironment $fixture $tempBase
}
Write-Host 'Deployment staging, validation, WhatIf, and owned-file behavior tests passed.'
