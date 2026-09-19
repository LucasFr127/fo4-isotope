# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repoRoot "scripts\internal\Project.Common.ps1")

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
    throw "Runtime status policy accepted a forbidden action: $Case"
}

foreach ($status in @(
    "runtime-pinned-unvalidated",
    "capture-complete",
    "release-approved"
)) {
    Assert-Fo4HdrRuntimeActionAllowed `
        -Status $status `
        -Action "developer-build"
    Assert-Fo4HdrRuntimeActionAllowed `
        -Status $status `
        -Action "runtime-identity-validation"
}

foreach ($status in @("primary-uninitialized", "planned")) {
    Assert-Rejected {
        Assert-Fo4HdrRuntimeActionAllowed `
            -Status $status `
            -Action "developer-build"
    } "$status developer build without override"
    if ($status -eq "planned") {
        Assert-Fo4HdrRuntimeActionAllowed `
            -Status $status `
            -Action "developer-build" `
            -AllowUnvalidatedRuntime
    } else {
        Assert-Rejected {
            Assert-Fo4HdrRuntimeActionAllowed `
                -Status $status `
                -Action "developer-build" `
                -AllowUnvalidatedRuntime
        } "$status developer build with override"
    }
    Assert-Rejected {
        Assert-Fo4HdrRuntimeActionAllowed `
            -Status $status `
            -Action "runtime-identity-validation" `
            -AllowUnvalidatedRuntime
    } "$status identity validation with developer override"
}

foreach ($status in @(
    "primary-uninitialized",
    "planned",
    "runtime-pinned-unvalidated",
    "capture-complete"
)) {
    Assert-Rejected {
        Assert-Fo4HdrRuntimeActionAllowed `
            -Status $status `
            -Action "release-packaging" `
            -AllowUnvalidatedRuntime
    } "$status release packaging"
}
Assert-Fo4HdrRuntimeActionAllowed `
    -Status "release-approved" `
    -Action "release-packaging"

Assert-Rejected {
    Assert-Fo4HdrRuntimeActionAllowed `
        -Status "captured" `
        -Action "developer-build" `
        -AllowUnvalidatedRuntime
} "unknown normalized status"

Write-Host "Runtime status action-policy regression tests passed."
