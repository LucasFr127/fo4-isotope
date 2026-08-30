# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.0

param([Parameter(Mandatory)][string]$Output, [switch]$PrintDependencyInfo)

$ErrorActionPreference = "Stop"
$projectRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, ".."))
$buildRoot = [IO.Path]::Combine($projectRoot, "build") + [IO.Path]::DirectorySeparatorChar
$outputPath = [IO.Path]::GetFullPath($Output)
if (-not $outputPath.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Shader output must be inside the project build directory."
}
$compiler = if ($env:FO4HDR_FXC) { $env:FO4HDR_FXC }
    else { (Get-Command fxc.exe -CommandType Application -ErrorAction Stop).Source }
$compilerItem = Get-Item -LiteralPath $compiler
$compilerPath = $compilerItem.FullName
if ($compilerPath -notmatch '(?i)[\\/]bin[\\/]10\.0\.26100\.0[\\/]x64[\\/]fxc\.exe$') {
    throw "Use the x64 Windows SDK 10.0.26100.0 FXC."
}
$version = $compilerItem.VersionInfo
$fileVersion = "{0}.{1}.{2}.{3}" -f $version.FileMajorPart, $version.FileMinorPart,
    $version.FileBuildPart, $version.FilePrivatePart
if ($fileVersion -cne "10.0.26100.8249") {
    throw "Unexpected FXC servicing version: $fileVersion"
}

$flags = @("/WX", "/Ges", "/Gis", "/O3")
$shaderDirectory = [IO.Path]::Combine($projectRoot, "build/include/fo4hdr-shaders")
$fixtureDirectory = [IO.Path]::GetDirectoryName($outputPath)
function Get-ShaderPrograms {
    $programs = [Collections.Generic.List[object]]::new()
    $programs.Add(@{
        Source = "tests/shader/shaders/TransferEvaluation.cs.hlsl"
        Profile = "cs_5_0"; Entry = "main"; Destination = $outputPath
    })
    foreach ($name in @("ToneMapA", "ToneMapB", "ToneMapNewA", "ToneMapNewB", "CreativeGradeLut",
        "TemporalAa", "TemporalAa2", "Fxaa", "Video")) {
        $programs.Add(@{
            Source = "shaders/fallout4/$name.ps.hlsl"; Profile = "ps_5_0"; Entry = "main"
            Symbol = "k$($name)Bytecode"; Destination = [IO.Path]::Combine($shaderDirectory, "$name.hpp")
        })
    }
    foreach ($entry in @("main", "PrepareBackdrop", "PrepareHud", "CopySdrCodes")) {
        $name = if ($entry -ceq "main") { "PresentPixels" } else { $entry }
        $programs.Add(@{
            Source = "shaders/output/PresentPixels.cs.hlsl"; Profile = "cs_5_0"; Entry = $entry
            Symbol = "k$($name)Bytecode"; Destination = [IO.Path]::Combine($shaderDirectory, "$name.hpp")
        })
    }
    $programs.Add(@{
        Source = "shaders/diagnostics/DeveloperExposureCapture.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"
        Symbol = "kDeveloperExposureCaptureBytecode"
        Destination = [IO.Path]::Combine($shaderDirectory, "DeveloperExposureCapture.hpp")
    })
    $programs.Add(@{
        Source = "shaders/armor/EncodedComposition.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"
        Symbol = "kArmorEncodedCompositionBytecode"
        Destination = [IO.Path]::Combine($shaderDirectory, "ArmorEncodedComposition.hpp")
    })
    $programs.Add(@{
        Source = "shaders/fallout4/NativeReferencePrepare.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"
        Symbol = "kNativeReferencePrepareBytecode"
        Destination = [IO.Path]::Combine($shaderDirectory, "NativeReferencePrepare.hpp")
    })
    foreach ($fixture in @(
        @{ Source = "tests/shader/shaders/HdrGradeTransfer.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"; Name = "HdrGradeTransfer" },
        @{ Source = "tests/shader/shaders/Gt7Evaluation.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"; Name = "Gt7Evaluation" },
        @{ Source = "tests/shader/shaders/OperatorLut.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"; Name = "OperatorLut" },
        @{ Source = "tests/shader/shaders/ArmorEncodedSource.ps.hlsl"; Profile = "ps_5_0"; Entry = "main"; Name = "ArmorEncodedSource" },
        @{ Source = "tests/shader/shaders/Fullscreen.vs.hlsl"; Profile = "vs_5_0"; Entry = "main"; Name = "Fullscreen" },
        @{ Source = "tests/shader/shaders/ScenePassSentinel.ps.hlsl"; Profile = "ps_5_0"; Entry = "main"; Name = "ScenePassSentinel" },
        @{ Source = "tests/shader/shaders/ScenePassSentinel.ps.hlsl"; Profile = "ps_5_0"; Entry = "plainMain"; Name = "ScenePassPlain" },
        @{ Source = "tests/shader/shaders/ScenePassSentinel.ps.hlsl"; Profile = "cs_5_0"; Entry = "computeMain"; Name = "ScenePassCompute" }
    )) {
        $programs.Add(@{
            Source = $fixture.Source; Profile = $fixture.Profile; Entry = $fixture.Entry
            Destination = [IO.Path]::Combine($fixtureDirectory, "$($fixture.Name).dxbc")
        })
    }
    return $programs.ToArray()
}
$programs = @(Get-ShaderPrograms)
function Get-ShaderInputs {
    $inputs = [Collections.Generic.List[string]]::new()
    foreach ($program in $programs) {
        $source = [IO.Path]::Combine($projectRoot, $program.Source)
        if (-not $inputs.Contains($source)) { $inputs.Add($source) }
    }
    foreach ($include in @("shaders/fallout4/GradeTransfer.hlsli",
        "shaders/fallout4/ImageControls.hlsli",
        "shaders/fallout4/TemporalIntermediate.hlsli", "shaders/fallout4/ToneMapBypassCommon.hlsli",
        "shaders/output/DisplayMath.hlsli", "shaders/output/SceneLayers.hlsli",
        "shaders/tonemap/Gt7.hlsli", "shaders/tonemap/OperatorLut.hlsli")) {
        $inputs.Add([IO.Path]::Combine($projectRoot, $include))
    }
    return $inputs.ToArray()
}
$inputs = @(Get-ShaderInputs)
if ($PrintDependencyInfo) {
    [ordered]@{
        compiler = [ordered]@{
            path = $compilerPath; version = $fileVersion
            sha256 = (Get-FileHash -LiteralPath $compilerPath -Algorithm SHA256).Hash
        }
        flags = @("/nologo") + $flags; inputs = $inputs
        outputs = @($programs | ForEach-Object { $_.Destination })
        outputRoot = $fixtureDirectory
        programs = @($programs | ForEach-Object {
            [ordered]@{ source = $_.Source; profile = $_.Profile; entry = $_.Entry; symbol = $_.Symbol }
        })
    } | ConvertTo-Json -Depth 5 -Compress
    return
}

function Invoke-ShaderCompilation {
    param([hashtable]$Program, [string]$Destination)

    $arguments = @("/nologo", "/T", $Program.Profile, "/E", $Program.Entry) + $flags
    if ($Program.Symbol) { $arguments += @("/Fh", $Destination, "/Vn", $Program.Symbol) }
    else { $arguments += @("/Fo", $Destination) }
    $arguments += [IO.Path]::Combine($projectRoot, $Program.Source)
    & $compilerPath @arguments
    if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($Destination)) {
        throw "Shader compilation failed: $($Program.Source) ($($Program.Entry))"
    }
}

function Publish-ChangedOutputs {
    param([object[]]$Staged)

    foreach ($item in $Staged) {
        $unchanged = [IO.File]::Exists($item.Destination) -and
            (Get-FileHash -LiteralPath $item.Destination -Algorithm SHA256).Hash -ceq
            (Get-FileHash -LiteralPath $item.Temporary -Algorithm SHA256).Hash
        if (-not $unchanged) {
            [IO.File]::Move($item.Temporary, $item.Destination, $true)
        }
    }
}

function Publish-ShaderPrograms {
    $staged = [Collections.Generic.List[object]]::new()
    try {
        foreach ($program in $programs) {
            $directory = [IO.Path]::GetDirectoryName($program.Destination)
            [IO.Directory]::CreateDirectory($directory) | Out-Null
            $temporary = [IO.Path]::Combine($directory, ".fo4hdr-" + [Guid]::NewGuid().ToString("N") + ".tmp")
            $staged.Add(@{ Temporary = $temporary; Destination = $program.Destination })
            Invoke-ShaderCompilation -Program $program -Destination $temporary
        }
        # Preserve good prior outputs until every program has compiled successfully.
        Publish-ChangedOutputs -Staged $staged.ToArray()
    }
    finally {
        foreach ($item in $staged) {
            if ([IO.File]::Exists($item.Temporary)) { [IO.File]::Delete($item.Temporary) }
        }
    }
}
Publish-ShaderPrograms
Write-Host "Compiled scene programs and shader checks with FXC $fileVersion"
