# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [string]$PackageRoot,

    [ValidateSet("fast", "full")]
    [string]$Scope = "full"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")
. (Join-Path $PSScriptRoot "internal/CommonLibIntegrity.ps1")
. (Join-Path $PSScriptRoot "internal/SourceLicense.ps1")
. (Join-Path $PSScriptRoot "internal/LicenseAuthorities.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$auditRoot = if ($PackageRoot) {
    [IO.Path]::GetFullPath($PackageRoot)
} else {
    $repoRoot
}
$auditScripts = Join-Path $auditRoot "scripts"
if ($PackageRoot -and $Scope -ne "full") {
    throw "Package audits require -Scope full."
}

$forbiddenNames = @(
    "Fallout4.exe",
    "f4se_loader.exe",
    "f4se_1_10_163.dll",
    "f4se_1_10_984.dll",
    "f4se_1_11_221.dll",
    "local.env.ps1"
)

$forbiddenExtensions = @(
    ".dll",
    ".dmp",
    ".rdc",
    ".dxbc",
    ".cso",
    ".exr",
    ".exe",
    ".exp",
    ".ilk",
    ".lib",
    ".log",
    ".mdmp",
    ".obj",
    ".pdb",
    ".etl",
    ".wer",
    ".ba2",
    ".esm",
    ".esp",
    ".esl"
)

$forbiddenNamePatterns = @(
    '(?i)^f4se_.*\.dll$',
    '(?i)^version-.*\.bin$'
)

function Assert-Fo4HdrPackageInventory {
    if ($PackageRoot) {
        $allowedPackageFiles = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase
        )
        foreach ($relative in $tracked) {
            [void]$allowedPackageFiles.Add($relative)
        }
        $sourceManifest = Get-Content -LiteralPath (
            Join-Path $auditRoot "third_party\xmake-source-manifest.json"
        ) -Raw | ConvertFrom-Json
        Add-Fo4HdrLinkedPackageFiles

        Assert-Fo4HdrPackagedFiles

    }
}

function Assert-Fo4HdrRegisteredPathFixture {
    foreach ($relative in @(
            "third_party/xmake-source-manifest.json"
    )) {
        if (-not (Test-Fo4HdrRegisteredThirdPartyPath $relative)) {
            throw "Third-party allowlist rejected its self-test path: $relative"
        }
    }
}

function Assert-Fo4HdrUnregisteredPathFixtures {
    foreach ($relative in @(
            "lib/unknown/foo.hpp",
            "third_party/unreviewed/foo.cpp"
    )) {
        if (Test-Fo4HdrRegisteredThirdPartyPath $relative) {
            throw "Third-party allowlist accepted its unsafe self-test path: $relative"
        }
    }
}

function Assert-Fo4HdrPolicyFiles {
    foreach ($relative in $required) {
        if (-not (Test-Path (Join-Path $auditRoot $relative))) {
            throw "Missing policy file: $relative"
        }
    }
}

function Get-Fo4HdrLicenseArtifactRoot {
    if ($PackageRoot) {
        return Join-Path `
        $auditRoot `
        "third_party\xmake-package-sources"
    } else {
        if (-not $env:FO4HDR_SOURCE_CACHE) {
            throw (
                "FO4HDR_SOURCE_CACHE is required to audit locked XMake " +
                "corresponding source."
            )
        }
        return $env:FO4HDR_SOURCE_CACHE
    }
}

function Copy-Fo4HdrReuseSelection {
    param([string]$SelectionRoot)

    foreach ($relative in $tracked) {
        # Dependencies are separate REUSE projects, including in expanded source archives.
        if ($relative.StartsWith('lib/', [StringComparison]::Ordinal) -or
            $relative.StartsWith('third_party/xmake-package-sources/', [StringComparison]::Ordinal)) {
            continue
        }
        $destination = Join-Path $SelectionRoot $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        [IO.File]::Copy((Join-Path $auditRoot $relative), $destination)
    }
}

function Invoke-Fo4HdrReuseLint {
    $command = if ($env:FO4HDR_REUSE) { $env:FO4HDR_REUSE } else { 'reuse' }
    $reuse = Get-Command $command -CommandType Application -ErrorAction SilentlyContinue
    if (-not $reuse) {
        throw 'Install REUSE and expose its executable through FO4HDR_REUSE or PATH.'
    }
    $version = & $reuse.Source --version | Out-String
    if ($LASTEXITCODE -ne 0 -or
        $version -notmatch ('(?m)^.*\bversion ' + [regex]::Escape($lock.toolchain.observed.reuse) + '\s*$')) {
        throw "REUSE version mismatch; expected $($lock.toolchain.observed.reuse)."
    }

    $selectionRoot = Join-Path ([IO.Path]::GetTempPath()) ('Fallout4HDR-reuse-' + [guid]::NewGuid().ToString('N'))
    try {
        Copy-Fo4HdrReuseSelection -SelectionRoot $selectionRoot
        & $reuse.Source --root $selectionRoot lint
        if ($LASTEXITCODE -ne 0) { throw 'REUSE lint failed for the selected public source.' }
    }
    finally {
        if (Test-Path -LiteralPath $selectionRoot) {
            $resolved = (Resolve-Path -LiteralPath $selectionRoot).Path
            if ($resolved -cne [IO.Path]::GetFullPath($selectionRoot)) {
                throw 'Unexpected REUSE temporary directory resolution.'
            }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}

function Assert-Fo4HdrSourceArtifacts {
    foreach ($relative in $tracked) {
        $reason = Get-Fo4HdrForbiddenArtifactReason $relative
        if ($reason) {
            throw "Forbidden source-tree artifact ($reason): $relative"
        }
    }
}

function Assert-Fo4HdrRegisteredSourcePaths {
    foreach ($relative in $tracked) {
        $normalized = $relative.Replace('\', '/')
        if (-not (Test-Fo4HdrRegisteredThirdPartyPath $normalized)) {
            throw (
                "Unregistered third-party surface: $normalized. Update " +
                "THIRD_PARTY_NOTICES.md and the audit allowlist after license " +
                "review."
            )
        }
    }
}

function Register-Fo4HdrGt7Sources {
    if ($gt7Authority.sourceUrl -cne $officialGt7Url) {
        throw "versions.lock.json does not identify the approved official GT7 URL."
    }
    if ((Test-Path $gt7Source) -xor (Test-Path $gt7Lock)) {
        throw "GT7 upstream source and lock must be present together."
    }
    if (($gt7Authority.status -eq "imported") -ne
        [bool](Test-Path -LiteralPath $gt7Source -PathType Leaf)) {
        throw (
            "Imported GT7 status requires the exact official corresponding " +
            "source and reviewed lock; an unimported status must not package them."
        )
    }
    if (Test-Path $gt7Source) {
        [void]$registeredThirdPartyPaths.Add($gt7SourceRelative)
        [void]$registeredThirdPartyPaths.Add($gt7LockRelative)
    }
}

function Test-Fo4HdrRuntimeAuthority {
    param($Actual, [Collections.IDictionary]$Expected)
    foreach ($field in $Expected.Keys) {
        if ($Actual.$field -ne $Expected[$field]) { return $false }
    }
    return $true
}
function Assert-Fo4HdrRuntimeSourceAuthorities {
    $runtimeManifest = Get-Content `
    -LiteralPath (Join-Path $auditRoot "runtime\1.10.163\manifest.json") `
    -Raw |
    ConvertFrom-Json
    $imageSpaceSources =
    $runtimeManifest.commonlibObservation.imageSpaceProfile.sources
    $relocationSource = $runtimeManifest.commonlibObservation.relocationSource
    $expectedAuthority = [ordered]@{
        repository = "https://github.com/Dear-Modding-FO4/commonlibf4.git"
        commit = $lock.commonlib.commit
        path = "include/RE/IDs.h"
    }
    if (-not (Test-Fo4HdrRuntimeAuthority $relocationSource $expectedAuthority)) {
        throw "The runtime relocation symbols are not bound to pinned Dear IDs.h."
    }
    $expectedAuthority = [ordered]@{
        repository = "https://github.com/Dear-Modding-FO4/commonlibf4.git"
        commit = $lock.commonlib.commit
    }
    if (-not (Test-Fo4HdrRuntimeAuthority $imageSpaceSources.commonlib $expectedAuthority)) {
        throw "The runtime image-space profile is not bound to pinned Dear CommonLibF4."
    }
    $expectedAuthority = [ordered]@{
        repository = "https://github.com/TES5Edit/TES5Edit.git"
        commit = "ca51ca1b67c2e84965c56d9163cec2acf20d6722"
    }
    if (-not (Test-Fo4HdrRuntimeAuthority $imageSpaceSources.xeditFieldAliases $expectedAuthority)) {
        throw "The runtime xEdit aliases are not bound to their reviewed source."
    }
}

function Add-Fo4HdrRegisteredPackageArchives {
    $sourceManifestForAllowlist = Get-Content -LiteralPath (
        Join-Path $auditRoot "third_party\xmake-source-manifest.json"
    ) -Raw | ConvertFrom-Json
    foreach ($package in $sourceManifestForAllowlist.packages) {
        foreach ($fileName in @(
                $package.sourceCacheFile,
                $package.recipeCacheFile
        )) {
            [void]$registeredThirdPartyPaths.Add(
                "third_party/xmake-package-sources/$fileName"
            )
        }
    }
}

function Assert-Fo4HdrSelectedSourceLicenses {
    foreach ($relative in $tracked) {
        $normalized = $relative.Replace('\', '/')
        if ($relative -like "third_party/*" -or
            $relative -like "lib/*") {
            continue
        }

        $extension =
        [IO.Path]::GetExtension(
            $relative
        ).ToLowerInvariant()

        if ($projectSourceExtensions -notcontains $extension) {
            continue
        }

        $full = Join-Path $auditRoot $relative
        $text = Get-Content $full -Raw
        Assert-Fo4HdrProductionSourceLicense `
        -RelativePath $normalized `
        -Text $text
    }
}

function Add-Fo4HdrLinkedPackageFiles {
    foreach ($package in @($sourceManifest.packages)) {
        foreach ($fileName in @(
                $package.sourceCacheFile,
                $package.recipeCacheFile
        )) {
            if ([IO.Path]::GetFileName([string]$fileName) -cne $fileName) {
                throw "Invalid linked-package archive name: $fileName"
            }
            [void]$allowedPackageFiles.Add(
                "third_party/xmake-package-sources/$fileName"
            )
        }
    }
}

function Assert-Fo4HdrPackagedFiles {
    foreach ($file in Get-ChildItem `
        -LiteralPath $auditRoot `
        -Recurse `
        -Force `
        -File) {
        $relative = [IO.Path]::GetRelativePath(
            $auditRoot,
            $file.FullName
        ).Replace('\', '/')
        $reason = Get-Fo4HdrForbiddenArtifactReason $relative
        if ($reason) {
            throw "Forbidden packaged artifact ($reason): $relative"
        }
        if (-not $allowedPackageFiles.Contains($relative)) {
            throw "Unexpected packaged output or dependency archive: $relative"
        }
    }
}

function Get-Fo4HdrForbiddenArtifactReason {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $normalized = $RelativePath.Replace('\', '/')
    $name = [IO.Path]::GetFileName($normalized)
    $extension = [IO.Path]::GetExtension($normalized).ToLowerInvariant()

    if ($forbiddenNames -contains $name) {
        return "forbidden runtime filename"
    }
    if ($forbiddenExtensions -contains $extension) {
        return "forbidden binary, dump, capture, or log extension"
    }
    foreach ($pattern in $forbiddenNamePatterns) {
        if ($name -match $pattern) {
            return "forbidden runtime filename pattern"
        }
    }

    return $null
}

function Assert-Fo4HdrSafeArtifactRules {
    foreach ($relative in @(
            "third_party/xmake-package-sources/spdlog-v1.16.0.zip",
            "third_party/xmake-package-sources/imgui-v1.92.6-docking.tar.gz",
            "src/plugin/PluginMain.cpp"
    )) {
        if (Get-Fo4HdrForbiddenArtifactReason $relative) {
            throw "Forbidden-artifact rule rejected its safe self-test path: $relative"
        }
    }
}

function Assert-Fo4HdrForbiddenArtifactRules {
    $unsafe = @(
        "Fallout4.exe",
        "f4se_1_10_777.dll",
        "version-1-10-163-0.bin",
        "tools/third-party.dll",
        "sample/session.dmp",
        "sample/session.log",
        "sample/Fallout4HDR.pdb"
    )
    foreach ($relative in $unsafe) {
        if (-not (Get-Fo4HdrForbiddenArtifactReason $relative)) {
            throw "Forbidden-artifact rule missed its self-test path: $relative"
        }
    }
    Assert-Fo4HdrSafeArtifactRules
}

Assert-Fo4HdrForbiddenArtifactRules

function Assert-Fo4HdrTextContainsAll {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string[]]$Required,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $text = Get-Content -LiteralPath $Path -Raw
    foreach ($value in $Required) {
        if (-not $text.Contains($value, [StringComparison]::Ordinal)) {
            throw "$Description is missing the required identity: $value"
        }
    }
}

$tracked = @(Get-Fo4HdrSourceTreeFiles -Root $auditRoot)

Assert-Fo4HdrPackageInventory

Assert-Fo4HdrSourceArtifacts

$registeredThirdPartyPaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal
)
foreach ($relative in @(
        "third_party/xmake-source-manifest.json"
)) {
    [void]$registeredThirdPartyPaths.Add($relative)
}

$gt7SourceRelative = "third_party/gt7/upstream/gt7_tone_mapping.cpp"
$gt7LockRelative = "third_party/gt7/UPSTREAM.lock.json"
$gt7Source = Join-Path $auditRoot $gt7SourceRelative
$gt7Lock = Join-Path $auditRoot $gt7LockRelative
$gt7Authority = (Get-Content -LiteralPath (
        Join-Path $auditRoot "versions.lock.json"
    ) -Raw | ConvertFrom-Json).gt7
$officialGt7Url =
"https://blog.selfshadow.com/publications/s2025-shading-course/" +
"pdi/supplemental/gt7_tone_mapping.cpp"
Register-Fo4HdrGt7Sources

if ($PackageRoot) {
    Add-Fo4HdrRegisteredPackageArchives
}

function Test-Fo4HdrRegisteredThirdPartyPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $normalized = $RelativePath.Replace('\', '/')
    if (-not $normalized.StartsWith("lib/", [StringComparison]::Ordinal) -and
        -not $normalized.StartsWith(
            "third_party/",
            [StringComparison]::Ordinal
    )) {
        return $true
    }
    if ($normalized.StartsWith(
            "lib/commonlibf4/",
            [StringComparison]::Ordinal
    )) {
        return $true
    }
    return $registeredThirdPartyPaths.Contains($normalized)
}

Assert-Fo4HdrRegisteredPathFixture
Assert-Fo4HdrUnregisteredPathFixtures

Assert-Fo4HdrRegisteredSourcePaths

$projectSourceExtensions = @(
    ".cpp",
    ".c",
    ".h",
    ".hpp",
    ".inl",
    ".hlsl",
    ".hlsli",
    ".ps1",
    ".lua",
    ".toml",
    ".yaml",
    ".yml"
)

$projectSpdxId =
"LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions"

Assert-Fo4HdrProductionSourceLicenseSelfTests

$noticesText = Get-Content (
    Join-Path $auditRoot "THIRD_PARTY_NOTICES.md"
) -Raw

Assert-Fo4HdrSelectedSourceLicenses

$required = @(
    "LICENSES\Apache-2.0.txt",
    "LICENSES\BSD-3-Clause.txt",
    "LICENSES\MIT.txt",
    "COPYING.GPL-3.0-or-later",
    "LICENSES\LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions.txt",
    "COPYING.CommonLib-Exceptions",
    "REUSE.toml",
    "THIRD_PARTY_NOTICES.md",
    "scripts\generate-combined-license.ps1"
)

Assert-Fo4HdrPolicyFiles

$lockPath = Join-Path $auditRoot "versions.lock.json"
$lock = Get-Content $lockPath -Raw | ConvertFrom-Json

Invoke-Fo4HdrReuseLint

Assert-Fo4HdrAcesAuthority

Assert-Fo4HdrAgxAuthority

if ($lock.project.license -ne "LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions") {
    throw "versions.lock.json contains the wrong project license ID."
}
if ($lock.gt7.status -eq "imported" -and
    -not (Test-Path -LiteralPath $gt7Source -PathType Leaf)) {
    throw "Imported GT7 requires its reviewed official source and source lock."
}

Assert-Fo4HdrSmoothTetraAuthority

$expectedGplHash =
"3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"
$expectedApacheHash =
"cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
$expectedBsdHash =
"48e1ebe3169067c37b5a497cf335c35e320c049ddfd226471f540e5f4c049d53"
$expectedMitHash =
"b05785f9f18e6716bab63424b11454513b9943a222595b70411009202fc592b5"
$expectedExceptionsHash =
"27a35e0c9d21cab2f07081660c9389c6bb990c210341aedbd2980de272e4893d"
$localGpl = Join-Path `
$auditRoot `
"COPYING.GPL-3.0-or-later"
$localApache = Join-Path $auditRoot "LICENSES\Apache-2.0.txt"
$localBsd = Join-Path $auditRoot "LICENSES\BSD-3-Clause.txt"
$localMit = Join-Path $auditRoot "LICENSES\MIT.txt"
$localExceptions = Join-Path $auditRoot "COPYING.CommonLib-Exceptions"

Assert-Fo4HdrLicenseTextHash $localGpl `
($expectedGplHash) `
"Project GPL text differs from its canonical upstream identity."
Assert-Fo4HdrLicenseTextHash $localApache `
($expectedApacheHash) `
"Project Apache-2.0 text differs from its canonical upstream identity."
Assert-Fo4HdrLicenseTextHash $localBsd `
($expectedBsdHash) `
"Project BSD-3-Clause text differs from its canonical identity."
Assert-Fo4HdrLicenseTextHash $localMit `
($expectedMitHash) `
"Project MIT text differs from its canonical SPDX identity."
if ((Get-Fo4HdrCanonicalTextSha256 $localExceptions) -ne $expectedExceptionsHash) {
    throw (
        "Project COPYING.CommonLib-Exceptions differs from its canonical project " +
        "identity."
    )
}
& (Join-Path $auditScripts "generate-combined-license.ps1") -Check

Assert-Fo4HdrGt7Import

Assert-Fo4HdrCommonLibLicense

if ($Scope -eq "fast") {
    Write-Host "Fast license and forbidden-artifact audit passed."
    return
}

Assert-Fo4HdrRuntimeSourceAuthorities

$noticesPath = Join-Path $auditRoot "THIRD_PARTY_NOTICES.md"
Assert-Fo4HdrTextContainsAll `
-Path $noticesPath `
-Required @(
    "CommonLibF4",
    "commonlib-shared",
    "GPL-3.0-or-later",
    "Exceptions"
) `
-Description "third-party notices"

$dependencyArtifactRoot = Get-Fo4HdrLicenseArtifactRoot

Test-Fo4HdrDependencySources `
-RepoRoot $auditRoot `
-ArtifactRoot $dependencyArtifactRoot

Write-Host "License and forbidden-artifact audit passed."
