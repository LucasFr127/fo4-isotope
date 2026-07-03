# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 7.0

param([Parameter(Mandatory)][string]$Output)

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

function Invoke-ShaderCompilation {
    param([hashtable]$Program, [string]$Destination)

    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Destination)) | Out-Null
    if ([IO.File]::Exists($Destination)) { Remove-Item -LiteralPath $Destination }
    $arguments = @("/nologo", "/T", $Program.Profile, "/E", $Program.Entry, "/WX", "/Ges", "/Gis", "/O3")
    if ($Program.Symbol) { $arguments += @("/Fh", $Destination, "/Vn", $Program.Symbol) }
    else { $arguments += @("/Fo", $Destination) }
    $arguments += [IO.Path]::Combine($projectRoot, $Program.Source)
    & $compilerPath @arguments
    if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($Destination)) {
        if ([IO.File]::Exists($Destination)) { Remove-Item -LiteralPath $Destination }
        throw "Shader compilation failed: $($Program.Source) ($($Program.Entry))"
    }
}

Invoke-ShaderCompilation -Program @{
    Source = "tests/shader/shaders/TransferEvaluation.cs.hlsl"; Profile = "cs_5_0"; Entry = "main"
} -Destination $outputPath

$shaderDirectory = [IO.Path]::Combine($projectRoot, "build/include/fo4hdr-shaders")
foreach ($name in @("ToneMapA", "ToneMapB", "ToneMapNewA", "ToneMapNewB", "CreativeGradeLut",
    "TemporalAa", "TemporalAa2")) {
    Invoke-ShaderCompilation -Program @{
        Source = "shaders/fallout4/$name.ps.hlsl"; Profile = "ps_5_0"; Entry = "main"; Symbol = "k$($name)Bytecode"
    } -Destination ([IO.Path]::Combine($shaderDirectory, "$name.hpp"))
}

$fixtureDirectory = [IO.Path]::GetDirectoryName($outputPath)
foreach ($fixture in @(
    @{ Source = "tests/shader/shaders/Fullscreen.vs.hlsl"; Profile = "vs_5_0"; Entry = "main"; Name = "Fullscreen" },
    @{ Source = "tests/shader/shaders/ScenePassSentinel.ps.hlsl"; Profile = "ps_5_0"; Entry = "main"; Name = "ScenePassSentinel" },
    @{ Source = "tests/shader/shaders/ScenePassSentinel.ps.hlsl"; Profile = "ps_5_0"; Entry = "plainMain"; Name = "ScenePassPlain" }
)) {
    Invoke-ShaderCompilation -Program $fixture `
        -Destination ([IO.Path]::Combine($fixtureDirectory, "$($fixture.Name).dxbc"))
}
Write-Host "Compiled scene programs and shader checks with FXC $fileVersion"
