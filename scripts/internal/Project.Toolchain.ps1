# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

Set-StrictMode -Version Latest

function Get-Fo4HdrToolchainPolicy {
    $policy = $env:FO4HDR_TOOLCHAIN_POLICY
    if ([string]::IsNullOrWhiteSpace($policy)) { return "pinned" }
    if ($policy -notin @("pinned", "hosted-ci")) {
        throw "Unknown toolchain policy: $policy"
    }
    return $policy
}

function Assert-Fo4HdrToolchainValue {
    param([string]$Key, [string]$Expected, [string]$Actual, [pscustomobject]$Lock)
    $policy = Get-Fo4HdrToolchainPolicy
    if ($Expected -match '^(FILL_|unknown$)' -or
        [string]::IsNullOrWhiteSpace($Expected)) {
        throw "$Key is not pinned. Run scripts/pin-toolchain.ps1."
    }
    $family = $null
    if ($policy -eq "hosted-ci") {
        $family = $Lock.toolchain.hostedCi.servicingFamilies.PSObject.Properties[$Key]
    }
    if ($null -ne $family) {
        Assert-Fo4HdrServicingVersion -Key $Key -Expected $Expected -Actual $Actual -Family $family.Value
        return
    }
    if ($policy -eq "hosted-ci" -and $Key -eq "git") {
        Assert-Fo4HdrAvailableGit -Actual $Actual -Requirement $Lock.toolchain.hostedCi.git
        return
    }
    if ($Expected -cne $Actual) {
        throw "$Key mismatch. Expected '$Expected'; observed '$Actual'."
    }
}

function Assert-Fo4HdrServicingVersion {
    param([string]$Key, [string]$Expected, [string]$Actual, [string]$Family)
    $version = [version]$Actual
    $inFamily = $Actual.StartsWith($Family + '.', [StringComparison]::Ordinal)
    if (-not $inFamily -or $version -lt [version]$Expected) {
        throw "$Key must be in family $Family, at least $Expected; observed '$Actual'."
    }
}

function Assert-Fo4HdrAvailableGit {
    param([string]$Actual, [string]$Requirement)
    if ($Requirement -cne "available") { throw "Unknown hosted-CI Git requirement: $Requirement" }
    if ($Actual -notmatch '^\d+\.\d+\.\d+(?:\.windows\.\d+)?$') {
        throw "Unable to validate hosted-CI Git version: '$Actual'."
    }
}

function Get-Fo4HdrMsvcToolset {
    param([pscustomobject]$Lock)
    $expected = [string]$Lock.toolchain.observed.msvc.toolsVersion
    if ((Get-Fo4HdrToolchainPolicy) -eq "pinned") { return $expected }
    $actual = ([string]$env:VCToolsVersion).TrimEnd('\', '/')
    Assert-Fo4HdrToolchainValue -Key "msvcTools" -Expected $expected -Actual $actual -Lock $Lock
    return $actual
}

function Assert-Fo4HdrConfiguredMsvc {
    param([pscustomobject]$Lock)
    $inspection = @'
import("core.project.config").load()
local tc = assert(import("core.tool.toolchain").load("msvc"))
local v = assert(tc:config("vcvars"), "MSVC has no configured environment")
print(import("core.base.json").encode({msvcTools = assert(v.VCToolsVersion), windowsSdk = assert(v.WindowsSDKVersion)}))
'@
    $result = xmake l -c $inspection
    if ($LASTEXITCODE -ne 0) { throw "Unable to inspect the configured MSVC toolchain." }
    $versions = $result -join "`n" | ConvertFrom-Json
    Assert-Fo4HdrToolchainValue -Key "msvcTools" -Expected $Lock.toolchain.observed.msvc.toolsVersion `
        -Actual $versions.msvcTools -Lock $Lock
    Assert-Fo4HdrToolchainValue -Key "windowsSdk" -Expected $Lock.toolchain.observed.windowsSdk.version `
        -Actual $versions.windowsSdk.TrimEnd('\', '/') -Lock $Lock
    Write-Host "Configured MSVC tools: $($versions.msvcTools); Windows SDK: $($versions.windowsSdk)."
}

function Get-Fo4HdrPowerShellPath {
    $configured = Resolve-Fo4HdrPowerShellCommand
    Assert-Fo4HdrPowerShellIdentity $configured
    return $configured
}

function Get-ActiveToolchainInfo {
    Assert-Fo4HdrToolchainCommands

    $buildTools = Get-Fo4HdrBuildToolVersions

    $clText = (& cl.exe 2>&1 | Out-String)
    $clMatch = [regex]::Match(
        $clText,
        'Version\s+([0-9.]+)\s+for\s+([A-Za-z0-9_]+)')
    if (-not $clMatch.Success) {
        throw "Unable to parse cl.exe version and target architecture."
    }

    $targetArchitecture = if ($env:VSCMD_ARG_TGT_ARCH) {
        $env:VSCMD_ARG_TGT_ARCH
    } else {
        $clMatch.Groups[2].Value
    }

    $hostArchitecture = if ($env:VSCMD_ARG_HOST_ARCH) {
        $env:VSCMD_ARG_HOST_ARCH
    } else {
        "unknown"
    }

    $visualStudio = Get-Fo4HdrVisualStudioInfo

    return [pscustomobject]@{
        VisualStudioProductLine = $visualStudio.ProductLine
        VisualStudioInstallationVersion =
            $visualStudio.InstallationVersion
        MsvcCompilerVersion = $clMatch.Groups[1].Value
        MsvcToolsVersion = (
            $env:VCToolsVersion ?? "unknown"
        ).TrimEnd([IO.Path]::DirectorySeparatorChar)
        HostArchitecture = $hostArchitecture
        TargetArchitecture = $targetArchitecture
        WindowsSdkVersion = (
            $env:WindowsSDKVersion ?? "unknown"
        ).TrimEnd([IO.Path]::DirectorySeparatorChar)
        XMakeVersion = $buildTools.XMake
        PowerShellVersion =
            $PSVersionTable.PSVersion.ToString()
        GitVersion = $buildTools.Git
    }
}

function Assert-X64Toolchain {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Toolchain
    )

    $validArch = @("x64", "amd64")

    if ($Toolchain.TargetArchitecture -notin $validArch) {
        throw (
            "The active MSVC target is '" +
            $Toolchain.TargetArchitecture +
            "'. Fallout4HDR requires x64. Open an x64 Visual " +
            "Studio developer environment."
        )
    }

    if ($Toolchain.HostArchitecture -notin $validArch) {
        throw (
            "The active MSVC host is '" +
            $Toolchain.HostArchitecture +
            "'. Use the x64 Native Tools environment."
        )
    }

    $sdkMatch = [regex]::Match(
        $Toolchain.WindowsSdkVersion,
        '^10\.0\.([0-9]+)(?:\.[0-9]+)?$')
    if (-not $sdkMatch.Success) {
        throw (
            "Unable to validate Windows SDK version '" +
            $Toolchain.WindowsSdkVersion +
            "'."
        )
    }

    if ([int]$sdkMatch.Groups[1].Value -lt 26100) {
        throw (
            "Windows SDK " +
            $Toolchain.WindowsSdkVersion +
            " is older than the supported project baseline 10.0.26100."
        )
    }
}

function Resolve-Fo4HdrPowerShellCommand {
    if (-not [string]::IsNullOrWhiteSpace($env:FO4HDR_PWSH)) {
        $configured = Resolve-Fo4HdrConfiguredPowerShell
    } else {
        $command = Get-Command pwsh -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $command) {
            throw "PowerShell 7 was not found through FO4HDR_PWSH or PATH."
        }
        $configured = $command.Source
    }
    return $configured
}

function Get-Fo4HdrVisualStudioInfo {
    $vsProductLine = "unknown"
    $vsInstallationVersion = "unknown"
    $selectedInstance = Get-Fo4HdrVisualStudioInstance
    if ($selectedInstance) {
        $vsInstallationVersion = $selectedInstance.installationVersion
        if ($selectedInstance.catalog.productLineVersion) {
            $vsProductLine = $selectedInstance.catalog.productLineVersion
        }
    }

    $vsProductLine = Resolve-Fo4HdrVisualStudioProductLine $vsProductLine

    return [pscustomobject]@{
        ProductLine = $vsProductLine
        InstallationVersion = $vsInstallationVersion
    }
}

function Select-Fo4HdrVisualStudioInstance {
    param([object[]]$Instances)
    $selectedInstance = $null
    if ($env:VSINSTALLDIR) {
        $activeInstall = Get-CanonicalPath $env:VSINSTALLDIR
        $selectedInstance = $instances |
            Where-Object {
                $_.installationPath -and
                (Get-CanonicalPath $_.installationPath) -eq
                    $activeInstall
            } |
            Select-Object -First 1

        if (-not $selectedInstance) {
            throw (
                "The active VSINSTALLDIR does not match any " +
                "Visual Studio instance reported by vswhere."
            )
        }
    } elseif ($instances.Count -eq 1) {
        $selectedInstance = $instances[0]
    } elseif ($instances.Count -gt 1) {
        throw (
            "Multiple Visual Studio installations were found, but " +
            "the active developer environment does not define " +
            "VSINSTALLDIR. Open the intended x64 Native Tools shell."
        )
    }

    return $selectedInstance
}

function Get-Fo4HdrBuildToolVersions {
    $gitText = (
        git --version |
        Select-Object -First 1
    ).Trim()
    $gitMatch = [regex]::Match(
        $gitText,
        'git version\s+(.+)$')
    if (-not $gitMatch.Success) {
        throw "Unable to parse Git version: $gitText"
    }

    $xmakeText = xmake --version | Out-String
    $xmakeMatch = [regex]::Match(
        $xmakeText,
        '\d+\.\d+\.\d+')
    if (-not $xmakeMatch.Success) {
        throw "Unable to parse XMake version."
    }

    return [pscustomobject]@{ Git = $gitMatch.Groups[1].Value; XMake = $xmakeMatch.Value }
}

function Get-Fo4HdrVisualStudioInstance {
    $vsWhere = Join-Path `
        ${env:ProgramFiles(x86)} `
        "Microsoft Visual Studio\Installer\vswhere.exe"

    if (Test-Path $vsWhere) {
        $jsonText = & $vsWhere `
            -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -format json `
            -utf8

        if ($LASTEXITCODE -eq 0 -and $jsonText) {
            $instances = @($jsonText | ConvertFrom-Json)
            $selectedInstance = $null

            $selectedInstance = Select-Fo4HdrVisualStudioInstance $instances

            return $selectedInstance
        }
    }
    return $null
}

function Resolve-Fo4HdrVisualStudioProductLine {
    param([string]$VsProductLine)
    if ($vsProductLine -eq "unknown" -and
        $env:VisualStudioVersion) {
        $vsMajor = [int](
            $env:VisualStudioVersion.Split('.')[0]
        )
        if ($vsMajor -eq 18) {
            $vsProductLine = "18"
        } else {
            $vsProductLine = $env:VisualStudioVersion
        }
    }

    return $vsProductLine
}

function Resolve-Fo4HdrConfiguredPowerShell {
    $configured = [IO.Path]::GetFullPath($env:FO4HDR_PWSH)
    if (Test-Path -LiteralPath $configured -PathType Container) {
        $configured = Join-Path $configured "pwsh.exe"
    }
    if (-not (Test-Path -LiteralPath $configured -PathType Leaf)) {
        throw "FO4HDR_PWSH does not identify pwsh.exe or its directory: $configured"
    }
    return $configured
}

function Assert-Fo4HdrToolchainCommands {
    foreach ($command in @("git", "xmake", "cl.exe")) {
        if (-not (
            Get-Command $command -ErrorAction SilentlyContinue
        )) {
            throw "Required command not found: $command"
        }
    }

    if ($PSVersionTable.PSVersion.Major -lt 7) {
        throw "PowerShell 7 is required."
    }

}

function Assert-Fo4HdrPowerShellIdentity {
    param([string]$Configured)
    if ([IO.Path]::GetFileName($configured) -ine "pwsh.exe") {
        throw "The selected PowerShell command is not pwsh.exe: $configured"
    }
    $identity = @(
        & $configured -NoProfile -NonInteractive -Command (
            '"{0}|{1}" -f $PSVersionTable.PSVersion.ToString(), ' +
            '[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture') |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    if ($LASTEXITCODE -ne 0 -or $identity.Count -ne 1) {
        throw "The selected PowerShell identity could not be verified."
    }
    $lock = Get-Content (Join-Path (Get-Fo4HdrRepoRoot) "versions.lock.json") `
        -Raw | ConvertFrom-Json
    $expectedVersion = [string]$lock.toolchain.observed.powerShell
    $parts = $identity[0].Split('|')
    if ($parts.Count -ne 2 -or $parts[1] -cne "X64") {
        throw (
            "PowerShell identity mismatch. Expected '$expectedVersion|X64'; " +
            "observed '$($identity[0])'."
        )
    }
    Assert-Fo4HdrToolchainValue -Key "powerShell" -Expected $expectedVersion `
        -Actual $parts[0] -Lock $lock
}
