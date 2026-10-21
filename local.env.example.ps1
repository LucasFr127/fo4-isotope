# SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
# Copy to local.env.ps1 and edit. Never commit or package local.env.ps1.

$env:FO4_ROOT = "D:\SteamLibrary\steamapps\common\Fallout 4"
$env:FO4_MOD_ROOT = "D:\Modding\MO2-FO4HDR\mods\FO4Isotope Development"
$env:FO4_CAPTURE_ROOT = "D:\Captures\FO4Isotope"

# Set this to a database in the MO2 Address Library folder; scripts read the
# selected runtime's database from that folder. Leave it empty to fall back to
# FO4_ROOT\Data\F4SE\Plugins.
$env:FO4_ADDRESS_LIBRARY_FILE = ""
# Example:
# $env:FO4_ADDRESS_LIBRARY_FILE =
#     "<MO2 mod root>\F4SE\Plugins\version-1-10-163-0.bin"

$env:FO4_LOG_ROOT = Join-Path `
    ([Environment]::GetFolderPath("MyDocuments")) `
    "My Games\Fallout4\F4SE"

$env:FO4HDR_RUNTIME = "1.10.163"
$env:FO4HDR_SOURCE_CACHE = "D:\DevDeps\FO4Isotope-Sources"

# Optional pinned PowerShell executable or its containing directory.
# Leave empty to resolve pwsh.exe through PATH.
$env:FO4HDR_PWSH = ""

# Optional REUSE executable. Leave empty to resolve reuse through PATH.
$env:FO4HDR_REUSE = ""
