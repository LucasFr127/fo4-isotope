# SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

Set-StrictMode -Version Latest

function Get-Fo4HdrRepoRoot {
    return (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
}

. (Join-Path $PSScriptRoot "Project.Process.ps1")
. (Join-Path $PSScriptRoot "Project.Toolchain.ps1")
. (Join-Path $PSScriptRoot "Project.Runtime.ps1")
. (Join-Path $PSScriptRoot "Project.Files.ps1")
. (Join-Path $PSScriptRoot "Project.Source.ps1")
