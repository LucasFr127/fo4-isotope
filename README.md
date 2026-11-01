<!-- SPDX-FileCopyrightText: FO4Isotope contributors -->
<!-- SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions -->

# FO4Isotope

FO4Isotope is an F4SE plugin, published as Isotope on Nexus Mods. It adds
modern tone mapping (GT7, AgX, ACES 2) and color grading to Fallout 4, in HDR
or SDR. On an HDR display, it also adds fast, native HDR10 output. On an SDR
display, you get the same tone mappers and color tools, with the defaults
matching the vanilla image until you change them. Adjust everything in the
in-game menu (default hotkey: F8).

Download: [Nexus Mods](https://www.nexusmods.com/fallout4/mods/109207)

## Features

### In HDR and SDR

- Tone mapping: GT7, AgX, and ACES 2, each with its own settings.
- Weather LUT response: Sets grading strength automatically from the game's
  weather data.
- Brightness and color: Exposure, contrast, highlights, shadows, blacks,
  saturation, temperature, and tint.
- Selective color: Hue, saturation and brightness for red, orange, yellow,
  green, cyan, blue, and magenta.
- Bloom: Adjustable strength.

### HDR only

- Native HDR10 output
- HDR grading: Two methods for extending the original grading into HDR.
- Bloom: Same as above, plus an experimental fix for the infamous white square
  bug that shows in HDR.
- Display: Separate peak brightness, menu & HUD white, and video white.

HDR and SDR keep separate settings, so switching modes never mixes them up.

## Requirements

- Fallout 4 AE, NG, or OG (1.11.240, 1.10.984, or 1.10.163)
- [F4SE](https://www.nexusmods.com/fallout4/mods/42147) and
  [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)
- For HDR, enable HDR in Windows. Isotope uses HDR whenever Windows HDR is on
  (not Auto HDR) and SDR otherwise

## Usage

Press **F8** to open the settings panel. HDR can be turned on or off there and
applies at the next game start. The shortcut and default settings can be
changed in `FO4Isotope.ini`.

## Building from source

FO4Isotope uses C++23, XMake, and Dear-Modding-FO4 CommonLibF4 on Windows x64.
The required tool versions are in `versions.lock.json`. PowerShell 7 and Git
can be newer patch releases.

Open the x64 Native Tools Command Prompt for VS 2026, start `pwsh`, and run:

```powershell
git submodule update --init --recursive -- lib/commonlibf4
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Mode debug
```

Build output goes to `build/`. The plugin is staged in
`dist/universal/Data/F4SE/Plugins/` as one DLL for OG, NG, and AE. XMake
downloads the pinned packages on the first build.

## Tests

After the Debug build, run the full suite, then build and test ReleaseDbg:

```powershell
.\scripts\test.ps1 -Runtime '1.10.163' -Mode debug -SkipConfigure -Scope all
.\scripts\build.ps1 -Mode releasedbg
.\scripts\test.ps1 -Runtime '1.10.163' -Mode releasedbg -SkipConfigure -Scope compiled
.\scripts\license-audit.ps1 -Scope fast
```

`-Runtime` selects the runtime profile under test: `1.10.163`, `1.10.984`, or
`1.11.240`. The license check requires REUSE on `PATH` or in `FO4HDR_REUSE`.

## License

Licensed under **GPL-3.0-or-later with the CommonLibF4
exceptions**. See the
[combined license](LICENSES/LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions.txt).
License texts are collected in `LICENSES/`. Third-party files retain their own
notices and licenses; attributions are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
