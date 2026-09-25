# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$Repository =
        "https://github.com/Dear-Modding-FO4/commonlibf4.git",

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string]$Commit
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
Push-Location $repoRoot

try {
    $expectedRepository =
        "https://github.com/Dear-Modding-FO4/commonlibf4.git"
    if ($Repository -ne $expectedRepository) {
        throw "Only the reviewed Dear-Modding-FO4 CommonLibF4 fork is supported."
    }

    if (-not (Test-Path ".git")) {
        throw "Initialize and commit the starter before adding a submodule."
    }

    $indexedCommonLib = @(git ls-files --stage -- "lib/commonlibf4")
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect the CommonLibF4 index entry."
    }
    if ($indexedCommonLib.Count -ne 0) {
        throw (
            "lib/commonlibf4 is already tracked. Initialize an existing " +
            "gitlink through the documented bootstrap workflow."
        )
    }

    if (Test-Path "lib/commonlibf4") {
        throw "lib/commonlibf4 already exists."
    }

    if (git status --porcelain --untracked-files=no) {
        throw "Commit or stash tracked changes before adding CommonLibF4."
    }

    git submodule add $Repository "lib/commonlibf4"
    if ($LASTEXITCODE -ne 0) {
        throw "git submodule add failed."
    }

    git -C "lib/commonlibf4" checkout --detach $Commit
    if ($LASTEXITCODE -ne 0) {
        git -C "lib/commonlibf4" fetch origin $Commit
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to fetch CommonLibF4 commit $Commit."
        }

        git -C "lib/commonlibf4" checkout --detach $Commit
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to check out CommonLibF4 commit $Commit."
        }
    }

    git submodule update --init --recursive -- "lib/commonlibf4"
    if ($LASTEXITCODE -ne 0) {
        throw "Recursive submodule initialization failed."
    }

    $commonCommit = (
        git -C "lib/commonlibf4" rev-parse HEAD
    ).Trim().ToLowerInvariant()

    if ($commonCommit -ne $Commit.ToLowerInvariant()) {
        throw (
            "CommonLibF4 checkout mismatch. Requested $Commit; " +
            "observed $commonCommit."
        )
    }

    $sharedRoot =
        "lib/commonlibf4/lib/commonlib-shared"
    $sharedCommit = (
        git -C $sharedRoot rev-parse HEAD
    ).Trim().ToLowerInvariant()
    $dearModdingUiApiRoot =
        "lib/commonlibf4/lib/dearmoddingui-api"
    $dearModdingUiApiCommit = (
        git -C $dearModdingUiApiRoot rev-parse HEAD
    ).Trim().ToLowerInvariant()

    $licensePath = "lib/commonlibf4/LICENSE"
    if (-not (Test-Path $licensePath)) {
        throw "CommonLibF4 LICENSE file is missing."
    }

    $localExceptions = Join-Path $repoRoot "COPYING.CommonLib-Exceptions"
    if (-not (Test-Path $localExceptions)) {
        throw "Project canonical license files are missing."
    }

    $upstreamLicenseHash =
        Get-Fo4HdrCanonicalTextSha256 $licensePath
    $expectedGplHash =
        "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"
    if ($upstreamLicenseHash -ne $expectedGplHash) {
        throw @"
The selected Dear-Modding CommonLibF4 commit does not carry the reviewed GPL
license identity. Review the upstream change and license policy deliberately.
"@
    }

    $commonExceptionsPath = "lib/commonlibf4/EXCEPTIONS"
    if (-not (Test-Path $commonExceptionsPath)) {
        throw "CommonLibF4 EXCEPTIONS file is missing."
    }
    $commonExceptionsHash =
        Get-Fo4HdrCanonicalTextSha256 $commonExceptionsPath

    $sharedRepository = (
        git -C "lib/commonlibf4" config `
            --file .gitmodules `
            --get submodule.lib/commonlib-shared.url
    ).Trim()
    $expectedSharedRepository =
        "https://github.com/Dear-Modding-FO4/commonlib-shared"
    if (($sharedRepository -replace '\.git$', '') -ne
        $expectedSharedRepository) {
        throw "CommonLibF4 does not reference Dear-Modding commonlib-shared."
    }

    $dearModdingUiApiRepository = (
        git -C "lib/commonlibf4" config `
            --file .gitmodules `
            --get submodule.lib/dearmoddingui-api.url
    ).Trim()
    $expectedDearModdingUiApiRepository =
        "https://github.com/Dear-Modding-FO4/DearModdingUI-API"
    if (($dearModdingUiApiRepository -replace '\.git$', '') -ne
        $expectedDearModdingUiApiRepository) {
        throw "CommonLibF4 does not reference DearModdingUI API."
    }

    $sharedExceptionsPath = Join-Path $sharedRoot "EXCEPTIONS"
    if (-not (Test-Path $sharedExceptionsPath)) {
        throw "commonlib-shared EXCEPTIONS file is missing."
    }
    $upstreamExceptionHash =
        Get-Fo4HdrCanonicalTextSha256 $sharedExceptionsPath
    $localExceptionHash =
        Get-Fo4HdrCanonicalTextSha256 $localExceptions
    if ($upstreamExceptionHash -ne $localExceptionHash) {
        throw @"
The project COPYING.CommonLib-Exceptions file does not match the selected commonlib-shared commit.
Review the upstream change, update the license policy and combined LicenseRef
text deliberately, then rerun this command.
"@
    }
    if ($commonExceptionsHash -ne $localExceptionHash) {
        throw @"
The project COPYING.CommonLib-Exceptions file does not match the selected CommonLibF4 commit.
Review the upstream change, update the license policy and combined LicenseRef
text deliberately, then rerun this command.
"@
    }

    $commonOriginalMitLicensePath =
        "lib/commonlibf4/res/license/MIT"
    if (-not (Test-Path $commonOriginalMitLicensePath)) {
        throw "CommonLibF4 original MIT license file is missing."
    }

    $lockPath = Join-Path $repoRoot "versions.lock.json"
    $lock = Get-Content $lockPath -Raw | ConvertFrom-Json
    $lock.commonlib.repository = $expectedRepository
    $lock.commonlib.commit = $commonCommit
    $lock.commonlib.commonlibSharedRepository =
        "https://github.com/Dear-Modding-FO4/commonlib-shared.git"
    $lock.commonlib.commonlibSharedCommit = $sharedCommit
    $lock.commonlib.dearModdingUiApiRepository =
        "https://github.com/Dear-Modding-FO4/DearModdingUI-API.git"
    $lock.commonlib.dearModdingUiApiCommit = $dearModdingUiApiCommit
    $sharedLicensePath =
        Join-Path $sharedRoot "LICENSE"
    if (-not (Test-Path $sharedLicensePath)) {
        throw "commonlib-shared LICENSE file is missing."
    }

    $lock.commonlib.licenseSha256 =
        $upstreamLicenseHash
    $lock.commonlib.exceptionsSha256 =
        $commonExceptionsHash
    $lock.commonlib.originalMitLicenseSha256 =
        Get-Fo4HdrCanonicalTextSha256 $commonOriginalMitLicensePath
    $lock.commonlib.commonlibSharedLicenseSha256 =
        Get-Fo4HdrCanonicalTextSha256 $sharedLicensePath
    $lock.commonlib.commonlibSharedExceptionsSha256 =
        $upstreamExceptionHash
    $dearModdingUiApiLicensePath =
        Join-Path $dearModdingUiApiRoot "LICENSE"
    if (-not (Test-Path $dearModdingUiApiLicensePath)) {
        throw "DearModdingUI API LICENSE file is missing."
    }
    $lock.commonlib.dearModdingUiApiLicenseSha256 =
        Get-Fo4HdrCanonicalTextSha256 $dearModdingUiApiLicensePath

    Write-Fo4HdrJsonFile `
        -Value $lock `
        -Path $lockPath `
        -SchemaPath (Join-Path $repoRoot "schemas\versions-lock.schema.json")

    & (Join-Path $PSScriptRoot "validate-manifests.ps1")

    Write-Host "Pinned CommonLibF4:       $commonCommit"
    Write-Host "Pinned commonlib-shared: $sharedCommit"
    Write-Host "Pinned DearModdingUI API: $dearModdingUiApiCommit"
    Write-Host (
        "Review and commit .gitmodules, submodule pointers, " +
        "and versions.lock.json."
    )
}
finally {
    Pop-Location
}
