# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
function Test-Fo4HdrAcesIdentityMismatch {
    return (-not (Test-Fo4HdrIdentityFields $aces2Lmt $expectedIdentity) -or
        [double]$aces2Lmt.inputShaperCenter -ne 2.4 -or
        [double]$aces2Lmt.inputShaperWidth -ne 3.0)
}

function Test-Fo4HdrSmoothTetraIdentityMismatch {
    return (-not (Test-Fo4HdrIdentityFields $smoothTetra $expectedIdentity) -or
        @($smoothTetra.sourceFiles).Count -ne 1 -or
        $smoothTetra.sourceFiles[0].path -cne
        "Effects/Smooth Tetra Skin.dctl" -or
        $smoothTetra.sourceFiles[0].sha256 -cne
        "3ab675ed68e3e2ea3f8d92dbeeb271a09b03183d303d5bc302ab37202a090497")
}

function Test-Fo4HdrGt7ManifestMismatch {
    return ($lock.gt7.status -ne "imported" -or
        $lock.gt7.sourceUrl -ne $officialGt7Url -or
        $lock.gt7.expectedSha256.ToLowerInvariant() -ne $gt7Hash -or
        $lock.gt7.importedSha256.ToLowerInvariant() -ne $gt7Hash)
}

function Test-Fo4HdrGt7LockMismatch {
    return ($gt7.sourceUrl -ne $officialGt7Url -or
        $gt7.finalUrl -ne $officialGt7Url -or
        $gt7.license -ne "MIT")
}

function Test-Fo4HdrIdentityFields {
    param($Actual, [Collections.IDictionary]$Expected)
    foreach ($field in $Expected.Keys) {
        if ($Actual.$field -cne $Expected[$field]) { return $false }
    }
    return $true
}

function Assert-Fo4HdrLicenseTextHash {
    param([string]$Path, [string]$Expected, [string]$Failure)
    if ((Get-Fo4HdrCanonicalTextSha256 $Path) -ne $Expected) { throw $Failure }
}

function Assert-Fo4HdrAcesNotices {
    $aces2LmtNoticeIdentities = @(
        "ACES 2 Contrast LMT",
        "Copyright Contributors to the ACES Project",
        "Apache-2.0"
    )
    foreach ($identity in $aces2LmtNoticeIdentities) {
        if (-not $noticesText.Contains($identity, [StringComparison]::Ordinal)) {
            throw "ACES 2 Contrast LMT notice is missing: $identity"
        }
    }

}

function Assert-Fo4HdrAcesAuthority {
    $expectedAces2Core = "ee380496a201fd008841ff9399b29241bcd27a3a"
    $expectedAces2Output = "90cadee61547c006ba8fad320ffacc171095bfad"
    $expectedAces2Lmt = "dc79dd74554e73a9e1518ec5f00ae0c1725f1686"
    $expectedAces2LmtSourceHash =
    "b5b1022c46758c02edd4c3967375d6daa0cd022e78c85962004a2f628318711f"
    $expectedAces2LmtLicenseHash =
    "43070e2d4e532684de521b885f385d0841030efa2b1a20bafb76133a5e1379c1"
    if ($lock.aces2.acesCoreCommit -cne $expectedAces2Core -or
        $lock.aces2.acesOutputCommit -cne $expectedAces2Output) {
        throw "The locked Academy ACES 2 source authority changed."
    }
    $aces2Lmt = $lock.aces2.contrastLmt
    $expectedIdentity = [ordered]@{
        status = "adapted"
        sourceRepository = "https://github.com/scottdyer/aces-look.git"
        sourceBranch = "dctl-contrast-lmt"
        sourceCommit = $expectedAces2Lmt
        sourcePath = "contrast/LMT_Test_OCIO_options.dctl"
        sourceSha256 = $expectedAces2LmtSourceHash
        license = "Apache-2.0"
        licensePath = "LICENSE"
        licenseSha256 = $expectedAces2LmtLicenseHash
        selectedVariant = "J curve 2 + M curve 2"
    }
    if (Test-Fo4HdrAcesIdentityMismatch) {
        throw "The locked ACES 2 Contrast LMT authority changed."
    }
    Assert-Fo4HdrAcesNotices
    foreach ($relative in @(
            "include/FO4Isotope/tonemap/Aces2Lmt.hpp",
            "src/tonemap/Aces2Lmt.cpp",
            "tests/support/reference/Aces2LmtSourceOracle.hpp",
            "tests/support/reference/Aces2LmtSourceOracle.cpp"
    )) {
        if (-not (Test-Path -LiteralPath (
                    Join-Path $auditRoot $relative
                ) -PathType Leaf)) {
            throw "ACES 2 Contrast LMT corresponding-source file is missing: $relative"
        }
    }
}

function Assert-Fo4HdrAgxNotices {
    foreach ($identity in @(
            "CustomToneMappingURP",
            "Copyright (c) 2025 Akihiro Noguchi",
            "Copyright 2013 Colour Developers",
            "MIT",
            "BSD-3-Clause"
    )) {
        if (-not $noticesText.Contains($identity, [StringComparison]::OrdinalIgnoreCase)) {
            throw "AgX notice is missing: $identity"
        }

    }
}

function Assert-Fo4HdrAgxAuthority {
    $expectedAgxFiles = @(
        @{
            Path = "Runtime/Baker/AgX/AgXToneMapping.cs"
            Blob = "f061e30054fcc24f149844ad7f863ba9e65cb903"
        },
        @{
            Path = "Runtime/Baker/AgX/AgXConstants.cs"
            Blob = "a418ed74a31780284f162477ce238b12412ddbb9"
        },
        @{
            Path = "Runtime/Baker/AgX/Sigmoid.cs"
            Blob = "64e64537c4fed400e71b404dfd2df53caac8149f"
        },
        @{
            Path = "Runtime/Baker/ColorUtility.cs"
            Blob = "533b91c279e8929cc9f2097edd647568de4e0685"
        }
    )
    $expectedIdentity = [ordered]@{
        status = "adapted"
        sourceRepository = "https://github.com/aki-null/CustomToneMappingURP.git"
        sourceTag = "1.2.4"
        sourceCommit = "ba6c68b5deafe4896115500d12cf976169d402ff"
    }
    if (-not (Test-Fo4HdrIdentityFields $lock.agx $expectedIdentity)) {
        throw "The locked CustomToneMappingURP AgX authority changed."
    }
    $lockedAgxFiles = @($lock.agx.sourceFiles)
    if ($lockedAgxFiles.Count -ne $expectedAgxFiles.Count) {
        throw "The locked CustomToneMappingURP AgX file set changed."
    }
    foreach ($expected in $expectedAgxFiles) {
        $entry = @($lockedAgxFiles | Where-Object {
                $_.path -ceq $expected.Path
        })
        if ($entry.Count -ne 1 -or
            $entry[0].gitBlobSha1 -cne $expected.Blob) {
            throw "The locked CustomToneMappingURP AgX blob changed: $($expected.Path)"
        }
    }
    Assert-Fo4HdrAgxNotices
}

function Assert-Fo4HdrSmoothTetraAuthority {
    $smoothTetra = $lock.smoothTetraSelectiveColor
    $expectedIdentity = [ordered]@{
        status = "adapted"
        sourceRepository = "https://github.com/thatcherfreeman/utility-dctls.git"
        sourceCommit = "a495531de3e4704a0a6e0113daa69be303172ed0"
        license = "MIT"
        licensePath = "LICENSE"
        licenseSha256 = "3b78284da28dedb1c33ff27961a9dede3e030bc3fdfeae2400afe4b22e6f87aa"
    }
    if (Test-Fo4HdrSmoothTetraIdentityMismatch) {
        throw "The locked Smooth Tetra selective-color authority changed."
    }
    foreach ($identity in @(
            "Smooth Tetra",
            "Copyright (c) 2023 Thatcher Freeman",
            "Juan Pablo Zambrano",
            "MIT"
    )) {
        if (-not $noticesText.Contains($identity,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Smooth Tetra notice is missing: $identity"
        }
    }
}

function Assert-Fo4HdrGt7SourceFiles {
    foreach ($requiredPath in @(
            $gt7SourceRelative,
            "src/tonemap/Gt7.cpp",
            "shaders/tonemap/Gt7.hlsli"
    )) {
        if (-not (Test-Path -LiteralPath (
                    Join-Path $auditRoot $requiredPath
                ) -PathType Leaf)) {
            throw "GT7 corresponding-source file is missing: $requiredPath"
        }

    }
}

function Assert-Fo4HdrGt7Import {
    if (-not (Test-Path $gt7Source)) { return }
    if (-not $noticesText.Contains('Copyright (c) 2025 Polyphony Digital Inc.', [StringComparison]::Ordinal)) {
        throw "GT7 copyright is missing from notices."
    }
    $gt7 = Get-Content $gt7Lock -Raw | ConvertFrom-Json
    if (-not $gt7.reviewed) {
        throw "GT7 source is present but not marked reviewed."
    }
    $gt7Hash = (Get-Sha256 $gt7Source).ToLowerInvariant()
    if ($gt7Hash -ne $gt7.sha256.ToLowerInvariant()) {
        throw "GT7 source hash does not match its reviewed lock."
    }
    if (Test-Fo4HdrGt7LockMismatch) {
        throw "GT7 reviewed lock does not identify the official MIT source."
    }
    if (Test-Fo4HdrGt7ManifestMismatch) {
        throw "GT7 source, upstream lock, and versions.lock.json disagree."
    }
    Assert-Fo4HdrGt7SourceFiles
}

function Assert-Fo4HdrCommonLibCommitLocks {
    if ($lock.commonlib.commit -notmatch '^[0-9a-fA-F]{40}$') {
        throw "CommonLibF4 submodule exists but its commit is not locked."
    }

    if ($lock.commonlib.commonlibSharedCommit -notmatch
        '^[0-9a-fA-F]{40}$') {
        throw "commonlib-shared exists but its commit is not locked."
    }
    if ($lock.commonlib.dearModdingUiApiCommit -notmatch
        '^[0-9a-fA-F]{40}$') {
        throw "DearModdingUI API exists but its commit is not locked."
    }
}

function Assert-Fo4HdrCommonLibLicenseTexts {
    $commonLicense = Join-Path $commonlib "LICENSE"
    if (-not (Test-Path $commonLicense)) {
        throw "Pinned CommonLibF4 LICENSE file is missing."
    }

    Assert-Fo4HdrLicenseTextHash $commonLicense `
    ($lock.commonlib.licenseSha256.ToLowerInvariant()) `
    "CommonLibF4 LICENSE hash differs from versions.lock.json."
    Assert-Fo4HdrLicenseTextHash $commonLicense `
    ($expectedGplHash) `
    "Dear-Modding CommonLibF4 GPL license identity is invalid."
    $commonExceptions = Join-Path $commonlib "EXCEPTIONS"
    Assert-Fo4HdrLicenseTextHash $commonExceptions `
    ($lock.commonlib.exceptionsSha256.ToLowerInvariant()) `
    "CommonLibF4 EXCEPTIONS differs from versions.lock.json."
    Assert-Fo4HdrLicenseTextHash $commonExceptions `
    ((Get-Fo4HdrCanonicalTextSha256 $localExceptions)) `
    "Project exception text differs from pinned CommonLibF4."
    $commonOriginalMitLicense = Join-Path $commonlib "res\license\MIT"
    Assert-Fo4HdrLicenseTextHash $commonOriginalMitLicense `
    ($lock.commonlib.originalMitLicenseSha256.ToLowerInvariant()) `
    "CommonLibF4 original MIT license differs from versions.lock.json."
}

function Assert-Fo4HdrCommonLibLicense {
    $commonlib = Join-Path $auditRoot "lib\commonlibf4"
    if (Test-Path $commonlib) {
        Assert-Fo4HdrCommonLibCommitLocks

        if ($PackageRoot) {
            Assert-Fo4HdrCommonLibRepositoryDeclarations `
            -RepoRoot $auditRoot `
            -Lock $lock
        } else {
            Assert-Fo4HdrCommonLibGitIntegrity `
            -RepoRoot $auditRoot `
            -Lock $lock

        }

        Assert-Fo4HdrCommonLibLicenseTexts

        $sharedRoot =
        Join-Path $commonlib "lib\commonlib-shared"
        $sharedLicense = Join-Path $sharedRoot "LICENSE"
        Assert-Fo4HdrLicenseTextHash $sharedLicense `
        ($lock.commonlib.commonlibSharedLicenseSha256.ToLowerInvariant()) `
        "commonlib-shared LICENSE hash differs from versions.lock.json."
        Assert-Fo4HdrLicenseTextHash $sharedLicense `
        ((Get-Fo4HdrCanonicalTextSha256 $localGpl)) `
        "Project GPL text differs from pinned commonlib-shared."

        $sharedExceptions = Join-Path $sharedRoot "EXCEPTIONS"
        Assert-Fo4HdrLicenseTextHash $sharedExceptions `
        ($lock.commonlib.commonlibSharedExceptionsSha256.ToLowerInvariant()) `
        "commonlib-shared EXCEPTIONS differs from versions.lock.json."
        Assert-Fo4HdrLicenseTextHash $sharedExceptions `
        ((Get-Fo4HdrCanonicalTextSha256 $localExceptions)) `
        "Project exception text differs from pinned commonlib-shared."

        $dearModdingUiApiRoot =
        Join-Path $commonlib "lib\dearmoddingui-api"
        $dearModdingUiApiLicense =
        Join-Path $dearModdingUiApiRoot "LICENSE"
        Assert-Fo4HdrLicenseTextHash $dearModdingUiApiLicense `
        ($lock.commonlib.dearModdingUiApiLicenseSha256.ToLowerInvariant()) `
        "DearModdingUI API LICENSE differs from versions.lock.json."
    }
}
