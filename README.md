<!-- SPDX-FileCopyrightText: FO4Isotope contributors -->
<!-- SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions -->

# FO4Isotope

FO4Isotope is an F4SE plugin for native HDR10 rendering and in-game color
controls in Fallout 4. It lets you adjust tone mapping, brightness, and color
from an in-game settings panel.

## Features

- HDR10 output with adjustable display peak, UI brightness, and video brightness.
- ACES 2, AgX, and GT7 tone mapping.
- Exposure, contrast, saturation, white balance, and selective color controls.
- Radial or Native Reference color grading, with controls for the game's
  grading, weather grading response, and bloom.

Press **Alt+H** to open the settings panel. The shortcut and default settings
can be changed in `FO4Isotope.ini`.

## Building from source

FO4Isotope uses C++23, XMake, and Dear-Modding-FO4 CommonLibF4 on Windows x64.
Use the tool versions recorded in `versions.lock.json`, including Visual Studio,
MSVC, the Windows SDK, XMake, PowerShell, Git, and REUSE.

Open PowerShell 7 in the matching Visual Studio x64 Native Tools environment,
then initialize the dependency, check the toolchain, and build:

```powershell
git -c core.autocrlf=false -c core.eol=lf submodule update --init --recursive -- lib/commonlibf4
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Mode debug
```

Build output goes to `build/`; plugin files are staged in `dist/universal/`.
XMake needs network access or a cache containing the pinned packages. The Git
options above preserve the dependency line endings required by bootstrap.

## Tests

After the Debug build, run the full suite, then build and test ReleaseDbg:

```powershell
.\scripts\test.ps1 -Runtime '1.10.163' -Mode debug -SkipConfigure -Scope all
.\scripts\build.ps1 -Mode releasedbg
.\scripts\test.ps1 -Runtime '1.10.163' -Mode releasedbg -SkipConfigure -Scope compiled
.\scripts\license-audit.ps1 -Scope fast
```

`-Runtime` selects the runtime profile under test: `1.10.163`, `1.10.984`, or
`1.11.240`. The suite covers CPU logic, D3D11 WARP shaders, and repository
checks. It can run without a Fallout 4 installation or a physical GPU; in-game
and display testing are separate. The license check requires REUSE on `PATH` or
in `FO4HDR_REUSE`.

## License

Licensed under **GPL-3.0-or-later with the CommonLibF4
exceptions**. See the
[combined license](LICENSES/LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions.txt).
License texts are collected in `LICENSES/`. Third-party files retain their own
notices and licenses.
