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
$shaderDirectory = [IO.Path]::Combine($projectRoot, "build/include/Fallout4HDR")
$fixtureDirectory = [IO.Path]::GetDirectoryName($outputPath)
$headerPath = [IO.Path]::Combine($shaderDirectory, "ShippingShaders.generated.hpp")
function Get-ShaderPrograms {
    $programs = [Collections.Generic.List[object]]::new()
    $programs.Add(@{
        Source = "tests/shader/shaders/TransferEvaluation.cs.hlsl"
        Profile = "cs_5_0"; Entry = "main"; Destination = $outputPath
    })
    foreach ($name in @("ToneMapA", "ToneMapB", "ToneMapNewA", "ToneMapNewB", "CreativeGradeLut",
        "NativeReferenceGrade", "TemporalAa", "TemporalAa2", "Fxaa", "Video")) {
        $programs.Add(@{
            Source = "shaders/fallout4/$name.ps.hlsl"; Profile = "ps_5_0"; Entry = "main"
            Symbol = "k$name"; Destination = [IO.Path]::Combine($fixtureDirectory, "$name.dxbc")
        })
    }
    foreach ($shader in @(
        @{ Source = "shaders/output/PresentPixels.cs.hlsl"; Name = "PresentPixels"; Symbol = "kPresentPixelsCs" },
        @{ Source = "shaders/diagnostics/DeveloperExposureCapture.cs.hlsl"; Name = "DeveloperExposureCapture"; Symbol = "kDeveloperExposureCaptureCs" },
        @{ Source = "shaders/armor/EncodedComposition.cs.hlsl"; Name = "ArmorEncodedComposition"; Symbol = "kArmorEncodedCompositionComputeShader" },
        @{ Source = "shaders/fallout4/NativeReferencePrepare.cs.hlsl"; Name = "NativeReferencePrepare"; Symbol = "kNativeReferencePrepare" }
    )) {
        $programs.Add(@{
            Source = $shader.Source; Profile = "cs_5_0"; Entry = "main"; Symbol = $shader.Symbol
            Destination = [IO.Path]::Combine($fixtureDirectory, "$($shader.Name).dxbc")
        })
    }
    foreach ($name in @("HdrGradeTransfer", "Gt7Evaluation", "OperatorLut")) {
        $programs.Add(@{
            Source = "tests/shader/shaders/$name.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"
            Destination = [IO.Path]::Combine($fixtureDirectory, "$name.dxbc")
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
        "shaders/fallout4/ImageControls.hlsli", "shaders/fallout4/SelectiveColor.hlsli",
        "shaders/fallout4/NativeReferenceGrade.hlsli", "shaders/fallout4/LiveSettingsConstants.hlsli",
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
        outputs = @($programs | ForEach-Object { $_.Destination }) + @($headerPath)
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
    $arguments += @("/Fo", $Destination)
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

function Add-ShaderBytecodeDeclaration {
    param([Text.StringBuilder]$Header, [string]$Symbol, [byte[]]$Bytes)
    [void]$Header.AppendLine("inline constexpr std::array<std::byte, $($Bytes.Length)> $Symbol = {")
    for ($offset = 0; $offset -lt $Bytes.Length; $offset += 12) {
        $count = [Math]::Min(12, $Bytes.Length - $offset)
        $values = for ($index = 0; $index -lt $count; ++$index) {
            "std::byte{0x$($Bytes[$offset + $index].ToString('X2'))}"
        }
        [void]$Header.AppendLine("    " + ($values -join ", ") + ",")
    }
    [void]$Header.AppendLine("};")
}

function Write-AggregateHeader {
    param([object[]]$Staged, [string]$Destination)
    $header = [Text.StringBuilder]::new()
    [void]$header.AppendLine("// Generated by scripts/compile-shaders.ps1. Do not edit.")
    [void]$header.AppendLine("#pragma once")
    [void]$header.AppendLine("#include <array>")
    [void]$header.AppendLine("#include <cstddef>")
    [void]$header.AppendLine("namespace fo4hdr::generated {")
    foreach ($item in $Staged) {
        if ($item.Symbol) {
            $bytes = [IO.File]::ReadAllBytes($item.Temporary)
            Add-ShaderBytecodeDeclaration -Header $header -Symbol $item.Symbol -Bytes $bytes
        }
    }
    [void]$header.AppendLine("}")
    [IO.File]::WriteAllText($Destination, $header.ToString().Replace("`r", ""), [Text.UTF8Encoding]::new($false))
}

function Publish-ShaderPrograms {
    $staged = [Collections.Generic.List[object]]::new()
    try {
        foreach ($program in $programs) {
            $directory = [IO.Path]::GetDirectoryName($program.Destination)
            [IO.Directory]::CreateDirectory($directory) | Out-Null
            $temporary = [IO.Path]::Combine($directory, ".fo4hdr-" + [Guid]::NewGuid().ToString("N") + ".tmp")
            $staged.Add(@{ Temporary = $temporary; Destination = $program.Destination; Symbol = $program.Symbol })
            Invoke-ShaderCompilation -Program $program -Destination $temporary
        }
        [IO.Directory]::CreateDirectory($shaderDirectory) | Out-Null
        $headerTemporary = [IO.Path]::Combine($shaderDirectory, ".fo4hdr-" + [Guid]::NewGuid().ToString("N") + ".tmp")
        $compiled = $staged.ToArray()
        $staged.Add(@{ Temporary = $headerTemporary; Destination = $headerPath })
        Write-AggregateHeader -Staged $compiled -Destination $headerTemporary
        # Preserve good prior outputs until every program and the header are ready.
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
