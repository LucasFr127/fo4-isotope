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
$source = [IO.Path]::Combine($projectRoot, "tests/shader/shaders/TransferEvaluation.cs.hlsl")
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
if ([IO.File]::Exists($outputPath)) { Remove-Item -LiteralPath $outputPath }
& $compilerPath /nologo /T cs_5_0 /E main /WX /Ges /Gis /O3 /Fo $outputPath $source
if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($outputPath)) {
    if ([IO.File]::Exists($outputPath)) { Remove-Item -LiteralPath $outputPath }
    throw "TransferEvaluation shader compilation failed."
}
Write-Host "Compiled TransferEvaluation.dxbc with FXC $fileVersion"
