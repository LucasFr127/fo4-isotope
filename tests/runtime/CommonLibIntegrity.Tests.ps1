# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot "scripts\internal\CommonLibIntegrity.ps1")

function Invoke-GitChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = @(git -C $Root @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed in '$Root': git $($Arguments -join ' '): $output"
    }
    return $output
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,

        [Parameter(Mandatory = $true)]
        [string]$Case
    )

    try {
        & $Action
    }
    catch {
        return
    }
    throw "CommonLib integrity regression was accepted: $Case"
}

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = Join-Path `
    $temporaryBase `
    ("Fallout4HDR-commonlib-integrity-" + [Guid]::NewGuid())
$temporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
if (-not $temporaryRoot.StartsWith(
    $temporaryBase,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw "CommonLib integrity test root escaped the temporary directory."
}
$commonRoot = Join-Path $temporaryRoot "lib\commonlibf4"
$sharedRoot = Join-Path $commonRoot "lib\commonlib-shared"
$dearModdingUiApiRoot = Join-Path $commonRoot "lib\dearmoddingui-api"
$commonRepository = "https://github.com/Dear-Modding-FO4/commonlibf4.git"
$sharedRepository = "https://github.com/Dear-Modding-FO4/commonlib-shared.git"
$dearModdingUiApiRepository =
    "https://github.com/Dear-Modding-FO4/DearModdingUI-API.git"

try {
    foreach ($root in @(
        $temporaryRoot,
        $commonRoot,
        $sharedRoot,
        $dearModdingUiApiRoot
    )) {
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        $null = Invoke-GitChecked $root @("init", "--quiet")
        $null = Invoke-GitChecked $root @("config", "user.name", "Fallout4HDR Tests")
        $null = Invoke-GitChecked $root @(
            "config",
            "user.email",
            "tests@fallout4hdr.invalid"
        )
    }

    Set-Content `
        -LiteralPath (Join-Path $sharedRoot "shared.txt") `
        -Value "shared" `
        -Encoding utf8NoBOM
    $null = Invoke-GitChecked $sharedRoot @("add", "shared.txt")
    $null = Invoke-GitChecked $sharedRoot @("commit", "--quiet", "-m", "shared")
    $sharedCommit = [string](
        Invoke-GitChecked $sharedRoot @("rev-parse", "HEAD")
    )
    $null = Invoke-GitChecked $sharedRoot @(
        "remote",
        "add",
        "origin",
        $sharedRepository
    )

    Set-Content `
        -LiteralPath (Join-Path $dearModdingUiApiRoot "api.txt") `
        -Value "api" `
        -Encoding utf8NoBOM
    $null = Invoke-GitChecked $dearModdingUiApiRoot @("add", "api.txt")
    $null = Invoke-GitChecked $dearModdingUiApiRoot @(
        "commit", "--quiet", "-m", "api"
    )
    $dearModdingUiApiCommit = [string](
        Invoke-GitChecked $dearModdingUiApiRoot @("rev-parse", "HEAD")
    )
    $null = Invoke-GitChecked $dearModdingUiApiRoot @(
        "remote", "add", "origin", $dearModdingUiApiRepository
    )

    $commonModules = @"
[submodule "lib/commonlib-shared"]
	path = lib/commonlib-shared
	url = $sharedRepository
[submodule "lib/dearmoddingui-api"]
	path = lib/dearmoddingui-api
	url = $dearModdingUiApiRepository
"@
    Set-Content `
        -LiteralPath (Join-Path $commonRoot ".gitmodules") `
        -Value $commonModules.TrimEnd() `
        -Encoding utf8NoBOM
    $null = Invoke-GitChecked $commonRoot @("add", ".gitmodules")
    $null = Invoke-GitChecked $commonRoot @(
        "update-index",
        "--add",
        "--cacheinfo",
        "160000,$sharedCommit,lib/commonlib-shared"
    )
    $null = Invoke-GitChecked $commonRoot @(
        "update-index",
        "--add",
        "--cacheinfo",
        "160000,$dearModdingUiApiCommit,lib/dearmoddingui-api"
    )
    $null = Invoke-GitChecked $commonRoot @(
        "commit",
        "--quiet",
        "-m",
        "common"
    )
    $commonCommit = [string](
        Invoke-GitChecked $commonRoot @("rev-parse", "HEAD")
    )
    $null = Invoke-GitChecked $commonRoot @(
        "remote",
        "add",
        "origin",
        $commonRepository
    )
    $null = Invoke-GitChecked $commonRoot @(
        "config",
        "submodule.lib/commonlib-shared.url",
        $sharedRepository
    )
    $null = Invoke-GitChecked $commonRoot @(
        "config",
        "submodule.lib/dearmoddingui-api.url",
        $dearModdingUiApiRepository
    )

    $rootModules = @"
[submodule "lib/commonlibf4"]
	path = lib/commonlibf4
	url = $commonRepository
"@
    Set-Content `
        -LiteralPath (Join-Path $temporaryRoot ".gitmodules") `
        -Value $rootModules.TrimEnd() `
        -Encoding utf8NoBOM
    $null = Invoke-GitChecked $temporaryRoot @("add", ".gitmodules")
    $null = Invoke-GitChecked $temporaryRoot @(
        "update-index",
        "--add",
        "--cacheinfo",
        "160000,$commonCommit,lib/commonlibf4"
    )
    $null = Invoke-GitChecked $temporaryRoot @(
        "commit",
        "--quiet",
        "-m",
        "root"
    )
    $null = Invoke-GitChecked $temporaryRoot @(
        "config",
        "submodule.lib/commonlibf4.url",
        $commonRepository
    )

    $lock = [pscustomobject]@{
        commonlib = [pscustomobject]@{
            repository = $commonRepository
            commit = $commonCommit
            commonlibSharedRepository = $sharedRepository
            commonlibSharedCommit = $sharedCommit
            dearModdingUiApiRepository = $dearModdingUiApiRepository
            dearModdingUiApiCommit = $dearModdingUiApiCommit
        }
    }
    if (-not (Test-Fo4HdrGitSubmoduleInitialized `
            -ParentRoot $temporaryRoot `
            -RelativePath 'lib/commonlibf4')) {
        throw 'Initialized CommonLibF4 fixture was not recognized.'
    }
    Assert-Fo4HdrCommonLibGitIntegrity `
        -RepoRoot $temporaryRoot `
        -Lock $lock

    $wrongLockRepository = $lock | ConvertTo-Json -Depth 5 | ConvertFrom-Json
    $wrongLockRepository.commonlib.repository =
        "https://github.com/libxse/commonlibf4.git"
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $wrongLockRepository
    } "obsolete locked repository"

    $null = Invoke-GitChecked $temporaryRoot @(
        "config",
        "submodule.lib/commonlibf4.url",
        "https://github.com/libxse/commonlibf4.git"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong registered CommonLibF4 repository"
    $null = Invoke-GitChecked $temporaryRoot @(
        "config",
        "submodule.lib/commonlibf4.url",
        $commonRepository
    )

    $null = Invoke-GitChecked $commonRoot @(
        "config",
        "submodule.lib/commonlib-shared.url",
        "https://github.com/libxse/commonlib-shared.git"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong registered commonlib-shared repository"
    $null = Invoke-GitChecked $commonRoot @(
        "config",
        "submodule.lib/commonlib-shared.url",
        $sharedRepository
    )

    $null = Invoke-GitChecked $commonRoot @(
        "config",
        "submodule.lib/dearmoddingui-api.url",
        "https://example.invalid/DearModdingUI-API.git"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong registered DearModdingUI API repository"
    $null = Invoke-GitChecked $commonRoot @(
        "config",
        "submodule.lib/dearmoddingui-api.url",
        $dearModdingUiApiRepository
    )

    $null = Invoke-GitChecked $commonRoot @(
        "remote",
        "set-url",
        "origin",
        "https://example.invalid/commonlibf4.git"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong CommonLibF4 checkout origin"
    $null = Invoke-GitChecked $commonRoot @(
        "remote",
        "set-url",
        "origin",
        $commonRepository
    )

    $null = Invoke-GitChecked $sharedRoot @(
        "remote",
        "set-url",
        "origin",
        "https://example.invalid/commonlib-shared.git"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong commonlib-shared checkout origin"
    $null = Invoke-GitChecked $sharedRoot @(
        "remote",
        "set-url",
        "origin",
        $sharedRepository
    )

    Add-Content `
        -LiteralPath (Join-Path $sharedRoot "shared.txt") `
        -Value "dirty"
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "dirty commonlib-shared worktree"
    $null = Invoke-GitChecked $sharedRoot @("restore", "shared.txt")

    Set-Content `
        -LiteralPath (Join-Path $commonRoot "dirty-header.h") `
        -Value "// dirty" `
        -Encoding utf8NoBOM
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "dirty CommonLibF4 worktree"
    Remove-Item -LiteralPath (Join-Path $commonRoot "dirty-header.h")

    Set-Content `
        -LiteralPath (Join-Path $sharedRoot "head-mismatch.txt") `
        -Value "new commit" `
        -Encoding utf8NoBOM
    $null = Invoke-GitChecked $sharedRoot @("add", "head-mismatch.txt")
    $null = Invoke-GitChecked $sharedRoot @(
        "commit",
        "--quiet",
        "-m",
        "wrong head"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong commonlib-shared HEAD"
    $null = Invoke-GitChecked $sharedRoot @(
        "reset",
        "--hard",
        "--quiet",
        $sharedCommit
    )

    $null = Invoke-GitChecked $temporaryRoot @(
        "update-index",
        "--cacheinfo",
        "160000,$sharedCommit,lib/commonlibf4"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibRootGitlink `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong indexed CommonLibF4 gitlink before initialization"
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong indexed CommonLibF4 gitlink"
    $null = Invoke-GitChecked $temporaryRoot @(
        "update-index",
        "--cacheinfo",
        "160000,$commonCommit,lib/commonlibf4"
    )

    $null = Invoke-GitChecked $commonRoot @(
        "update-index",
        "--cacheinfo",
        "160000,$commonCommit,lib/commonlib-shared"
    )
    Assert-Rejected {
        Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $temporaryRoot `
            -Lock $lock
    } "wrong indexed commonlib-shared gitlink"
    $null = Invoke-GitChecked $commonRoot @(
        "update-index",
        "--cacheinfo",
        "160000,$sharedCommit,lib/commonlib-shared"
    )

    Assert-Fo4HdrCommonLibGitIntegrity `
        -RepoRoot $temporaryRoot `
        -Lock $lock

    $archiveRoot = Join-Path $temporaryRoot 'source-archive'
    $archiveCommon = Join-Path $archiveRoot 'lib/commonlibf4'
    New-Item -ItemType Directory -Force -Path $archiveCommon | Out-Null
    Copy-Item -LiteralPath (Join-Path $temporaryRoot '.gitmodules') -Destination $archiveRoot
    Copy-Item -LiteralPath (Join-Path $commonRoot '.gitmodules') -Destination $archiveCommon
    Assert-Fo4HdrCommonLibRepositoryDeclarations -RepoRoot $archiveRoot -Lock $lock
    Assert-Rejected {
        Assert-Fo4HdrCommonLibRootGitlink -RepoRoot $archiveRoot -Lock $lock
    } 'source archive is not an indexed checkout'
    $archiveModules = Join-Path $archiveRoot '.gitmodules'
    (Get-Content -LiteralPath $archiveModules -Raw).Replace(
        'Dear-Modding-FO4/commonlibf4', 'unapproved/commonlibf4'
    ) | Set-Content -LiteralPath $archiveModules -Encoding utf8NoBOM
    Assert-Rejected {
        Assert-Fo4HdrCommonLibRepositoryDeclarations -RepoRoot $archiveRoot -Lock $lock
    } 'wrong source-archive CommonLibF4 declaration'

    Remove-Item -LiteralPath $commonRoot -Recurse -Force
    New-Item -ItemType Directory -Path $commonRoot | Out-Null
    Assert-Fo4HdrCommonLibRootGitlink `
        -RepoRoot $temporaryRoot `
        -Lock $lock
    if (Test-Fo4HdrGitSubmoduleInitialized `
            -ParentRoot $temporaryRoot `
            -RelativePath 'lib/commonlibf4') {
        throw 'Uninitialized CommonLibF4 gitlink was not recognized.'
    }

    Write-Host "CommonLib recursive dependency integrity tests passed."
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
