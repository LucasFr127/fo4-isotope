# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
[CmdletBinding()]
param(
    [ValidateSet("1.10.163", "1.10.984", "1.11.221")]
    [string]$Runtime = $env:FO4HDR_RUNTIME,

    [ValidateSet("debug", "releasedbg")]
    [string]$Mode = "debug",

    [switch]$AllowUnvalidatedRuntime,

    [switch]$DisableFrameTrace,

    [ValidateSet("all", "compiled", "repository")]
    [string]$Scope = "all",

    [switch]$SkipConfigure,

    [ValidateRange(1, 2147483)]
    [int]$TestTimeoutSeconds = 600,

    [ValidateSet("all", "unit", "shader")]
    [string]$Suite = "all",

    [ValidateNotNullOrEmpty()]
    [string]$Filter,

    [switch]$List
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$runCompiled = $Scope -in @("all", "compiled")
$runRepository = $Scope -in @("all", "repository")
if (($Suite -ne "all" -or $Filter -or $List) -and $Scope -ne "compiled") {
    throw "-Suite, -Filter and -List require -Scope compiled."
}
if ($Filter -and ($Suite -eq "all" -or $List)) {
    throw "-Filter requires -Suite unit or shader and cannot be combined with -List."
}
$testArguments = if ($List) { @("--list") } elseif ($Filter) { @("--filter", $Filter) } else { @() }

if ($runCompiled) {
    if ($SkipConfigure) {
        $configurationPath = Join-Path `
            $repoRoot `
            ".xmake\windows\x64\xmake.conf"
        if (-not (Test-Path -LiteralPath $configurationPath -PathType Leaf)) {
            throw "-SkipConfigure requires an existing x64 XMake configuration."
        }
        $configuration = Get-Content -LiteralPath $configurationPath -Raw
        $lock = Get-Content (Join-Path $repoRoot "versions.lock.json") -Raw | ConvertFrom-Json
        $expected = [ordered]@{
            arch = '"x64"'
            plat = '"windows"'
            toolchain = '"msvc"'
            vs_toolset = '"' + (Get-Fo4HdrMsvcToolset -Lock $lock) + '"'
            vs_sdkver = '"' + $lock.toolchain.observed.windowsSdk.version + '"'
            mode = '"' + $Mode + '"'
            fo4hdr_runtime = '"' + $Runtime + '"'
            fo4hdr_allow_unvalidated_runtime = if ($AllowUnvalidatedRuntime) {
                'true'
            } else {
                'false'
            }
            fo4hdr_build_tests = 'true'
            fo4hdr_frame_trace = if ($DisableFrameTrace) { 'false' } else { 'true' }
            commonlib_ini = 'false'
            commonlib_json = 'false'
            commonlib_toml = 'false'
            commonlib_xbyak = 'false'
        }
        foreach ($entry in $expected.GetEnumerator()) {
            $pattern = '(?m)^\s*' + [regex]::Escape($entry.Key) +
                '\s*=\s*' + [regex]::Escape($entry.Value) + '\s*,?\s*$'
            if ($configuration -notmatch $pattern) {
                throw (
                    "Existing XMake configuration does not match " +
                    "$($entry.Key)=$($entry.Value). Run build.ps1 for " +
                    "the requested configuration before test.ps1 " +
                    "-SkipConfigure."
                )
            }
        }
        Assert-Fo4HdrConfiguredMsvc -Lock $lock
    } else {
        & (Join-Path $PSScriptRoot "configure.ps1") `
            -Runtime $Runtime `
            -Mode $Mode `
            -AllowUnvalidatedRuntime:$AllowUnvalidatedRuntime `
            -DisableFrameTrace:$DisableFrameTrace
    }
} elseif ($SkipConfigure) {
    throw "-SkipConfigure is unnecessary with -Scope repository."
}

Push-Location $repoRoot
try {
    if ($runCompiled) {
        $targets = switch ($Suite) {
            "unit" { "fo4hdr-tests" }
            "shader" { "fo4hdr-shader-tests" }
            "all" { "fo4hdr-tests"; "fo4hdr-shader-tests" }
        }
        foreach ($target in $targets) {
            xmake build $target
            if ($LASTEXITCODE -ne 0) { throw "Test build failed: $target" }
            $targetInfo = xmake show -t $target --format=json
            if ($LASTEXITCODE -ne 0) { throw "Unable to inspect test target: $target" }
            $targetFile = ($targetInfo -join "`n" | ConvertFrom-Json).targetfile
            if (-not $targetFile) { throw "Test target has no executable: $target" }
            # Launch the resolved executable directly to preserve its skip exit code.
            Invoke-Fo4HdrTestProcess -FilePath ([IO.Path]::GetFullPath($targetFile, $repoRoot)) `
                -Arguments $testArguments -AllowSkip:($target -eq 'fo4hdr-shader-tests') `
                -WorkingDirectory $repoRoot -TimeoutSeconds $TestTimeoutSeconds
        }
    }

    if ($runRepository) {
        $approvedPowerShell = Get-Fo4HdrPowerShellPath
        $processOptions = @{
            FilePath = $approvedPowerShell
            WorkingDirectory = $repoRoot
            TimeoutSeconds = $TestTimeoutSeconds
        }
        foreach ($relativePath in @(
            "tests\runtime\AddressLibraryV0.Tests.ps1",
            "tests\license\CanonicalLicense.Tests.ps1",
            "tests\source\SourceText.Tests.ps1",
            "tests\runtime\RuntimeStatusPolicy.Tests.ps1",
            "tests\runtime\RuntimePinning.Tests.ps1",
            "tests\source\StagePolicy.Tests.ps1",
            "tests\runtime\ImageSpaceProfile.Tests.ps1",
            "tests\runtime\CommonLibRelocationProfile.Tests.ps1",
            "tests\runtime\CommonLibIntegrity.Tests.ps1",
            "tests\source\PluginBoundarySource.Tests.ps1",
            "tests\source\ArchitectureLayout.Tests.ps1",
            "tests\source\SourceTree.Tests.ps1",
            "tests\source\SourceArchive.Tests.ps1",
            "tests\source\TestProcess.Tests.ps1",
            "tests\source\ToolchainPolicy.Tests.ps1",
            "tests\runtime\MonitorNativeSession.Tests.ps1"
        )) {
            Invoke-Fo4HdrTestProcess @processOptions -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', (Join-Path $repoRoot $relativePath))
        }

        Invoke-Fo4HdrTestProcess @processOptions -Arguments @(
            '-NoProfile', '-NonInteractive', '-File',
            (Join-Path $repoRoot 'tests/source/ArchitectureLayout.Tests.ps1'), '-SelfTest')

        Invoke-Fo4HdrTestProcess @processOptions -Arguments @(
            '-NoProfile', '-NonInteractive', '-File',
            (Join-Path $repoRoot 'scripts/verify-plugin-metadata.ps1'), '-SelfTest')

        $auditScript = Join-Path $repoRoot 'scripts/audit-f4se-project.ps1'
        Invoke-Fo4HdrTestProcess @processOptions -Arguments @(
            '-NoProfile', '-NonInteractive', '-File', $auditScript, '-SelfTest')
        Invoke-Fo4HdrTestProcess @processOptions -Arguments @(
            '-NoProfile', '-NonInteractive', '-File', $auditScript,
            '-Root', $repoRoot, '-FailOn', 'high', '-ReRoot', 'src/game',
            '-F4seFile', (
                "src/plugin/PluginMain.cpp," +
                "runtime/1.10.163/PluginVersion.cpp," +
                "runtime/1.10.984/PluginVersion.cpp," +
                "runtime/1.11.221/PluginVersion.cpp"
            ), '-PluginPchFile', 'src/plugin/PluginPch.hpp')
    }
}
finally {
    Pop-Location
}
