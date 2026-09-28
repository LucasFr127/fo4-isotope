# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

function ConvertTo-Fo4HdrCanonicalGitRepository {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Repository
    )

    return $Repository.Trim() -replace '(?i)\.git$', ''
}

function Get-Fo4HdrGitValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $value = @(git -C $WorkingDirectory @Arguments 2>$null)
    if ($LASTEXITCODE -ne 0 -or $value.Count -ne 1) {
        throw "Unable to read $Description."
    }
    if ([string]::IsNullOrWhiteSpace($value[0])) {
        throw "Unable to read $Description."
    }

    return $value[0].Trim()
}

function Assert-Fo4HdrGitRepository {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Expected,

        [Parameter(Mandatory = $true)]
        [string]$Observed
    )

    $canonicalExpected =
        ConvertTo-Fo4HdrCanonicalGitRepository $Expected
    $canonicalObserved =
        ConvertTo-Fo4HdrCanonicalGitRepository $Observed
    if ($canonicalObserved -cne $canonicalExpected) {
        throw (
            "$Name repository mismatch. Expected '$Expected'; " +
            "observed '$Observed'."
        )
    }
}

function Assert-Fo4HdrGitRemote {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Expected
    )

    $repositories = @(git -C $Root remote get-url --all origin 2>$null)
    if ($LASTEXITCODE -ne 0 -or $repositories.Count -eq 0) {
        throw "Unable to read the $Name fetch URLs."
    }
    foreach ($repository in $repositories) {
        Assert-Fo4HdrGitRepository `
            -Name "$Name fetch" `
            -Expected $Expected `
            -Observed $repository
    }
}

function Assert-Fo4HdrCommonLibRepositoryDeclarations {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [psobject]$Lock
    )

    $expectedShared =
        'https://github.com/Dear-Modding-FO4/commonlib-shared.git'
    $expectedDearModdingUiApi =
        'https://github.com/Dear-Modding-FO4/DearModdingUI-API.git'

    Assert-Fo4HdrCommonLibRootDeclaration `
        -RepoRoot $RepoRoot `
        -Lock $Lock
    Assert-Fo4HdrGitRepository `
        -Name 'Locked commonlib-shared' `
        -Expected $expectedShared `
        -Observed ([string]$Lock.commonlib.commonlibSharedRepository)
    Assert-Fo4HdrGitRepository `
        -Name 'Locked DearModdingUI API' `
        -Expected $expectedDearModdingUiApi `
        -Observed ([string]$Lock.commonlib.dearModdingUiApiRepository)

    $commonRoot = Join-Path $RepoRoot 'lib\commonlibf4'

    $sharedDeclaration = Get-Fo4HdrGitValue `
        -WorkingDirectory $commonRoot `
        -Arguments @(
            'config',
            '--file',
            (Join-Path $commonRoot '.gitmodules'),
            '--get',
            'submodule.lib/commonlib-shared.url'
        ) `
        -Description 'the tracked commonlib-shared repository declaration'
    Assert-Fo4HdrGitRepository `
        -Name 'Tracked commonlib-shared' `
        -Expected $expectedShared `
        -Observed $sharedDeclaration

    $dearModdingUiApiDeclaration = Get-Fo4HdrGitValue `
        -WorkingDirectory $commonRoot `
        -Arguments @(
            'config',
            '--file',
            (Join-Path $commonRoot '.gitmodules'),
            '--get',
            'submodule.lib/dearmoddingui-api.url'
        ) `
        -Description 'the tracked DearModdingUI API repository declaration'
    Assert-Fo4HdrGitRepository `
        -Name 'Tracked DearModdingUI API' `
        -Expected $expectedDearModdingUiApi `
        -Observed $dearModdingUiApiDeclaration
}

function Assert-Fo4HdrCommonLibRootDeclaration {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [psobject]$Lock
    )

    $expectedCommon =
        'https://github.com/Dear-Modding-FO4/commonlibf4.git'
    Assert-Fo4HdrGitRepository `
        -Name 'Locked CommonLibF4' `
        -Expected $expectedCommon `
        -Observed ([string]$Lock.commonlib.repository)

    $lockedCommit = [string]$Lock.commonlib.commit
    if ($lockedCommit -notmatch '^[0-9a-fA-F]{40}$') {
        throw 'CommonLibF4 is not pinned in versions.lock.json.'
    }

    $rootDeclaration = Get-Fo4HdrGitValue `
        -WorkingDirectory $RepoRoot `
        -Arguments @(
            'config',
            '--file',
            (Join-Path $RepoRoot '.gitmodules'),
            '--get',
            'submodule.lib/commonlibf4.url'
        ) `
        -Description 'the tracked CommonLibF4 repository declaration'
    Assert-Fo4HdrGitRepository `
        -Name 'Tracked CommonLibF4' `
        -Expected $expectedCommon `
        -Observed $rootDeclaration
}

function Assert-Fo4HdrCommonLibRootGitlink {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [psobject]$Lock
    )

    Assert-Fo4HdrCommonLibRootDeclaration -RepoRoot $RepoRoot -Lock $Lock
    Assert-Fo4HdrGitlink `
        -ParentRoot $RepoRoot `
        -RelativePath 'lib/commonlibf4' `
        -ExpectedCommit ([string]$Lock.commonlib.commit) `
        -Name 'CommonLibF4'
}

function Assert-Fo4HdrGitlink {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ParentRoot,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedCommit,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $record = Get-Fo4HdrGitValue `
        -WorkingDirectory $ParentRoot `
        -Arguments @('ls-files', '--stage', '--', $RelativePath) `
        -Description "$Name indexed gitlink"
    $escapedPath = [regex]::Escape($RelativePath.Replace('\', '/'))
    if ($record -notmatch "^160000 ([0-9a-fA-F]{40}) 0\t$escapedPath$") {
        throw "$Name is not an indexed Git submodule."
    }

    if ($Matches[1].ToLowerInvariant() -ne
        $ExpectedCommit.ToLowerInvariant()) {
        throw (
            "$Name indexed gitlink mismatch. Expected $ExpectedCommit; " +
            "observed $($Matches[1])."
        )
    }
}

function Test-Fo4HdrGitSubmoduleInitialized {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ParentRoot,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $submoduleRoot = Join-Path $ParentRoot $RelativePath
    $gitMarker = Join-Path $submoduleRoot '.git'
    return Test-Path -LiteralPath $gitMarker
}

function Assert-Fo4HdrCleanGitWorktree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $status = @(
        git -C $Root status `
            --porcelain `
            --untracked-files=all `
            --ignore-submodules=none
    )
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect the $Name worktree."
    }
    if ($status.Count -ne 0) {
        throw "$Name worktree is not clean: $($status -join '; ')"
    }
}

function Assert-Fo4HdrCommonLibRegisteredRepositories {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [psobject]$Lock
    )

    $commonRoot = Join-Path $RepoRoot 'lib\commonlibf4'
    $sharedRoot = Join-Path $commonRoot 'lib\commonlib-shared'
    $dearModdingUiApiRoot = Join-Path $commonRoot 'lib\dearmoddingui-api'
    Assert-Fo4HdrGitRepository `
        -Name 'Registered CommonLibF4' `
        -Expected ([string]$Lock.commonlib.repository) `
        -Observed (Get-Fo4HdrGitValue `
            -WorkingDirectory $RepoRoot `
            -Arguments @(
                'config',
                '--get',
                'submodule.lib/commonlibf4.url'
            ) `
            -Description 'the registered CommonLibF4 repository')
    Assert-Fo4HdrGitRemote `
        -Root $commonRoot `
        -Name 'CommonLibF4 origin' `
        -Expected ([string]$Lock.commonlib.repository)

    Assert-Fo4HdrGitRepository `
        -Name 'Registered commonlib-shared' `
        -Expected ([string]$Lock.commonlib.commonlibSharedRepository) `
        -Observed (Get-Fo4HdrGitValue `
            -WorkingDirectory $commonRoot `
            -Arguments @(
                'config',
                '--get',
                'submodule.lib/commonlib-shared.url'
            ) `
            -Description 'the registered commonlib-shared repository')
    Assert-Fo4HdrGitRemote `
        -Root $sharedRoot `
        -Name 'commonlib-shared origin' `
        -Expected ([string]$Lock.commonlib.commonlibSharedRepository)

    Assert-Fo4HdrGitRepository `
        -Name 'Registered DearModdingUI API' `
        -Expected ([string]$Lock.commonlib.dearModdingUiApiRepository) `
        -Observed (Get-Fo4HdrGitValue `
            -WorkingDirectory $commonRoot `
            -Arguments @(
                'config',
                '--get',
                'submodule.lib/dearmoddingui-api.url'
            ) `
            -Description 'the registered DearModdingUI API repository')
    Assert-Fo4HdrGitRemote `
        -Root $dearModdingUiApiRoot `
        -Name 'DearModdingUI API origin' `
        -Expected ([string]$Lock.commonlib.dearModdingUiApiRepository)

}

function Assert-Fo4HdrCommonLibCommits {
    param([string]$RepoRoot, [psobject]$Lock)

    $commonRoot = Join-Path $RepoRoot 'lib\commonlibf4'
    $sharedRoot = Join-Path $commonRoot 'lib\commonlib-shared'
    $dearModdingUiApiRoot = Join-Path $commonRoot 'lib\dearmoddingui-api'
    Assert-Fo4HdrGitlink `
        -ParentRoot $RepoRoot `
        -RelativePath 'lib/commonlibf4' `
        -ExpectedCommit ([string]$Lock.commonlib.commit) `
        -Name 'CommonLibF4'
    Assert-Fo4HdrGitlink `
        -ParentRoot $commonRoot `
        -RelativePath 'lib/commonlib-shared' `
        -ExpectedCommit ([string]$Lock.commonlib.commonlibSharedCommit) `
        -Name 'commonlib-shared'
    Assert-Fo4HdrGitlink `
        -ParentRoot $commonRoot `
        -RelativePath 'lib/dearmoddingui-api' `
        -ExpectedCommit ([string]$Lock.commonlib.dearModdingUiApiCommit) `
        -Name 'DearModdingUI API'

    $commonHead = Get-Fo4HdrGitValue `
        -WorkingDirectory $commonRoot `
        -Arguments @('rev-parse', 'HEAD^{commit}') `
        -Description 'the CommonLibF4 HEAD commit'
    if ($commonHead.ToLowerInvariant() -ne
        $Lock.commonlib.commit.ToLowerInvariant()) {
        throw 'CommonLibF4 HEAD differs from versions.lock.json.'
    }

    $sharedHead = Get-Fo4HdrGitValue `
        -WorkingDirectory $sharedRoot `
        -Arguments @('rev-parse', 'HEAD^{commit}') `
        -Description 'the commonlib-shared HEAD commit'
    if ($sharedHead.ToLowerInvariant() -ne
        $Lock.commonlib.commonlibSharedCommit.ToLowerInvariant()) {
        throw 'commonlib-shared HEAD differs from versions.lock.json.'
    }

    $dearModdingUiApiHead = Get-Fo4HdrGitValue `
        -WorkingDirectory $dearModdingUiApiRoot `
        -Arguments @('rev-parse', 'HEAD^{commit}') `
        -Description 'the DearModdingUI API HEAD commit'
    if ($dearModdingUiApiHead.ToLowerInvariant() -ne
        $Lock.commonlib.dearModdingUiApiCommit.ToLowerInvariant()) {
        throw 'DearModdingUI API HEAD differs from versions.lock.json.'
    }

}

function Assert-Fo4HdrCommonLibGitIntegrity {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][psobject]$Lock
    )

    Assert-Fo4HdrCommonLibRepositoryDeclarations -RepoRoot $RepoRoot -Lock $Lock
    $commonRoot = Join-Path $RepoRoot 'lib\commonlibf4'
    $sharedRoot = Join-Path $commonRoot 'lib\commonlib-shared'
    $dearModdingUiApiRoot = Join-Path $commonRoot 'lib\dearmoddingui-api'
    foreach ($root in @($commonRoot, $sharedRoot, $dearModdingUiApiRoot)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            throw 'CommonLibF4 recursive submodules are not initialized.'
        }
    }
    Assert-Fo4HdrCommonLibRegisteredRepositories -RepoRoot $RepoRoot -Lock $Lock
    Assert-Fo4HdrCommonLibCommits -RepoRoot $RepoRoot -Lock $Lock
    Assert-Fo4HdrCleanGitWorktree `
        -Root $dearModdingUiApiRoot `
        -Name 'DearModdingUI API'
    Assert-Fo4HdrCleanGitWorktree `
        -Root $sharedRoot `
        -Name 'commonlib-shared'
    Assert-Fo4HdrCleanGitWorktree `
        -Root $commonRoot `
        -Name 'CommonLibF4'
}
