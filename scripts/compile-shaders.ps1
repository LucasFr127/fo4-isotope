# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$FxcPath,
    [switch]$PrintDependencyInfo
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "internal/Project.Common.ps1")

$repoRoot = Get-Fo4HdrRepoRoot
$buildRoot = Join-Path $repoRoot "build"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $buildRoot "generated"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-IsSameOrChildPath $OutputDirectory $buildRoot)) {
    throw "Generated shaders must remain below the repository build directory."
}

$lock = Get-Content (Join-Path $repoRoot "versions.lock.json") -Raw |
    ConvertFrom-Json
$expectedSdk = [string]$lock.toolchain.observed.windowsSdk.version

function Test-ShaderSpecificationTestOnly {
    param([System.Collections.IDictionary]$Specification)
    return $Specification.Contains("TestOnly") -and $Specification.TestOnly
}
function Resolve-ValidatedFxcPath {
    param([string]$Candidate)
    $resolved = [IO.Path]::GetFullPath($candidate)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "fxc.exe does not exist: $resolved"
    }
    if ($resolved -notmatch '(?i)[\\/]x64[\\/]fxc\.exe$') {
        throw "The approved x64 Windows SDK fxc.exe is required: $resolved"
    }

    $normalized = $resolved.Replace('/', '\')
    $versionSegment = "\bin\$expectedSdk\x64\fxc.exe"
    if (-not $normalized.EndsWith(
        $versionSegment,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw (
            "fxc.exe is not from the locked Windows SDK $expectedSdk`: " +
            $resolved
        )
    }
    return $resolved
}

function Assert-RequiredShaderTokens {
    param([System.Collections.IDictionary]$Specification, [string]$ReflectionText)
    foreach ($token in @($specification.RequiredReflectionTokens)) {
        if (-not [string]::IsNullOrWhiteSpace($token) -and
            $reflectionText -notmatch [regex]::Escape($token)) {
            throw (
                "Reflection for $($specification.Source) is missing " +
                "required binding/signature token: $token"
            )
        }
    }
}

function Get-LockedFxc {
    $kitsRoot = Get-ItemPropertyValue `
        -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" `
        -Name KitsRoot10 `
        -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($kitsRoot)) {
        $lockedCandidate = Join-Path $kitsRoot (
            "bin\{0}\x64\fxc.exe" -f $expectedSdk)
        if (Test-Path -LiteralPath $lockedCandidate -PathType Leaf) {
            return $lockedCandidate
        }
    }
}

function Get-FxcCandidate {
    $candidate = $null
    if (-not [string]::IsNullOrWhiteSpace($FxcPath)) {
        $candidate = $FxcPath
    } elseif (-not [string]::IsNullOrWhiteSpace($env:FO4HDR_FXC)) {
        $candidate = $env:FO4HDR_FXC
    } elseif (-not [string]::IsNullOrWhiteSpace($env:WindowsSdkDir) -and
        -not [string]::IsNullOrWhiteSpace($env:WindowsSDKVersion)) {
        $sdkVersion = $env:WindowsSDKVersion.TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar)
        $candidate = Join-Path $env:WindowsSdkDir (
            "bin\{0}\x64\fxc.exe" -f $sdkVersion)
    } else {
        $candidate = Get-LockedFxc
    }

    return $candidate
}

function Resolve-Fo4HdrFxc {
    $candidate = Get-FxcCandidate
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $command = Get-Command fxc.exe -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $command) {
            throw (
                "fxc.exe was not found. Enter the approved x64 MSVC " +
                "developer environment or set FO4HDR_FXC to the approved " +
                "Windows SDK x64 compiler."
            )
        }
        $candidate = $command.Source
    }

    return Resolve-ValidatedFxcPath $candidate
}

$fxc = Resolve-Fo4HdrFxc
$specifications = @(
    [ordered]@{ Name = "ArmorEncodedCompositionComputeShader"; Symbol = "kArmorEncodedCompositionComputeShader"; Source = "shaders/armor/EncodedComposition.cs.hlsl"; Profile = "cs_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB13[1]", "dcl_uav_typed_texture2d (float,float,float,float) u0", "ld_uav_typed") },
    [ordered]@{ Name = "DeveloperExposureCaptureCs"; Symbol = "kDeveloperExposureCaptureCs"; Source = "shaders/diagnostics/DeveloperExposureCapture.cs.hlsl"; Profile = "cs_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[2]", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_uav_typed_texture2d (float,float,float,float) u0") },
    [ordered]@{ Name = "ToneMapA"; Symbol = "kToneMapA"; Source = "shaders/fallout4/ToneMapA.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[5]", "dcl_constantbuffer CB13[4]", "dcl_resource_texture2d (float,float,float,float) t17", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "ToneMapB"; Symbol = "kToneMapB"; Source = "shaders/fallout4/ToneMapB.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[6]", "dcl_constantbuffer CB13[4]", "dcl_resource_texture2d (float,float,float,float) t17", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "ToneMapNewA"; Symbol = "kToneMapNewA"; Source = "shaders/fallout4/ToneMapNewA.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[5]", "dcl_constantbuffer CB13[4]", "dcl_resource_texture2d (float,float,float,float) t17", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "ToneMapNewB"; Symbol = "kToneMapNewB"; Source = "shaders/fallout4/ToneMapNewB.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[6]", "dcl_constantbuffer CB13[4]", "dcl_resource_texture2d (float,float,float,float) t17", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "CreativeGradeLut"; Symbol = "kCreativeGradeLut"; Source = "shaders/fallout4/CreativeGradeLut.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[2]", "dcl_constantbuffer CB13[20]", "dcl_sampler s0", "dcl_sampler s3", "dcl_sampler s4", "dcl_sampler s5", "dcl_sampler s6", "dcl_sampler s7", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture3d (float,float,float,float) t3", "dcl_resource_texture3d (float,float,float,float) t4", "dcl_resource_texture3d (float,float,float,float) t5", "dcl_resource_texture3d (float,float,float,float) t6", "dcl_resource_texture3d (float,float,float,float) t7", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "NativeReferenceGrade"; Symbol = "kNativeReferenceGrade"; Source = "shaders/fallout4/NativeReferenceGrade.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[2]", "dcl_constantbuffer CB13[20]", "dcl_sampler s0", "dcl_sampler s7", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture3d (float,float,float,float) t3", "dcl_resource_texture3d (float,float,float,float) t7", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "NativeReferencePrepare"; Symbol = "kNativeReferencePrepare"; Source = "shaders/fallout4/NativeReferencePrepare.cs.hlsl"; Profile = "cs_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[2]", "dcl_resource_texture3d (float,float,float,float) t0", "dcl_resource_texture3d (float,float,float,float) t3", "dcl_uav_typed_texture3d (float,float,float,float) u0", "dcl_thread_group 8, 8, 8") },
    [ordered]@{ Name = "TemporalAa"; Symbol = "kTemporalAa"; Source = "shaders/fallout4/TemporalAa.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[6]", "dcl_constantbuffer CB13[1]", "dcl_sampler s0", "dcl_sampler s1", "dcl_sampler s2", "dcl_sampler s3", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture2d (float,float,float,float) t1", "dcl_resource_texture2d (float,float,float,float) t2", "dcl_resource_texture2d (float,float,float,float) t3", "dcl_output o0.xyzw", "dcl_output o1.xyzw") },
    [ordered]@{ Name = "TemporalAa2"; Symbol = "kTemporalAa2"; Source = "shaders/fallout4/TemporalAa2.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[6]", "dcl_constantbuffer CB13[1]", "dcl_sampler s0", "dcl_sampler s1", "dcl_sampler s2", "dcl_sampler s3", "dcl_sampler s4", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture2d (float,float,float,float) t1", "dcl_resource_texture2d (float,float,float,float) t2", "dcl_resource_texture2d (float,float,float,float) t3", "dcl_resource_texture2d (float,float,float,float) t4", "dcl_output o0.xyzw", "dcl_output o1.xyzw") },
    [ordered]@{ Name = "Fxaa"; Symbol = "kFxaa"; Source = "shaders/fallout4/Fxaa.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB2[1]", "dcl_sampler s0", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "Video"; Symbol = "kVideo"; Source = "shaders/fallout4/Video.ps.hlsl"; Profile = "ps_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB13[2]", "dcl_sampler s0", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "PresentPixelsCs"; Symbol = "kPresentPixelsCs"; Source = "shaders/output/PresentPixels.cs.hlsl"; Profile = "cs_5_0"; RequiredReflectionTokens = @("dcl_constantbuffer CB0[3]", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture2d (float,float,float,float) t3", "dcl_uav_typed_texture2d (float,float,float,float) u0", "dcl_thread_group 8, 8, 1") }
)
$testSpecifications = @(
    [ordered]@{ Name = "LayerEvaluationCs"; Symbol = "kLayerEvaluationCs"; Source = "tests/shader/shaders/LayerEvaluation.cs.hlsl"; Profile = "cs_5_0"; TestOnly = $true },
    [ordered]@{ Name = "TransferEvaluationCs"; Symbol = "kTransferEvaluationCs"; Source = "tests/shader/shaders/TransferEvaluation.cs.hlsl"; Profile = "cs_5_0"; TestOnly = $true },
    [ordered]@{ Name = "ArmorEncodedSourcePs"; Symbol = "kArmorEncodedSourcePs"; Source = "tests/shader/shaders/ArmorEncodedSource.ps.hlsl"; Profile = "ps_5_0"; TestOnly = $true; RequiredReflectionTokens = @("dcl_constantbuffer CB0[1]", "dcl_output o0.xyzw") },
    [ordered]@{ Name = "HdrGradeTransferCs"; Symbol = "kHdrGradeTransferCs"; Source = "tests/shader/shaders/HdrGradeTransfer.cs.hlsl"; Profile = "cs_5_0"; TestOnly = $true; RequiredReflectionTokens = @("dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture2d (float,float,float,float) t2", "dcl_uav_typed_texture2d (float,float,float,float) u0") },
    [ordered]@{ Name = "NativeGradeSamplingPs"; Symbol = "kNativeGradeSamplingPs"; Source = "tests/shader/shaders/NativeGradeSampling.ps.hlsl"; Profile = "ps_5_0"; TestOnly = $true; RequiredReflectionTokens = @("dcl_constantbuffer CB2[2]", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture3d (float,float,float,float) t3", "dcl_resource_texture3d (float,float,float,float) t6", "dcl_sampler s3", "dcl_sampler s6", "dcl_output o0.xyzw", "dcl_output o1.xyzw") },
    [ordered]@{ Name = "OperatorLutCs"; Symbol = "kOperatorLutCs"; Source = "tests/shader/shaders/OperatorLut.cs.hlsl"; Profile = "cs_5_0"; TestOnly = $true; RequiredReflectionTokens = @("dcl_constantbuffer CB0[2]", "dcl_sampler s7", "dcl_resource_texture2d (float,float,float,float) t0", "dcl_resource_texture3d (float,float,float,float) t7", "dcl_uav_typed_texture2d (float,float,float,float) u0") },
    [ordered]@{ Name = "FullscreenVs"; Symbol = "kFullscreenVs"; Source = "tests/shader/shaders/Fullscreen.vs.hlsl"; Profile = "vs_5_0"; TestOnly = $true; RequiredReflectionTokens = @("dcl_input_sgv v0.x, vertex_id", "dcl_output_siv o0.xyzw, position", "dcl_output o1.xyzw", "dcl_output o2.xyzw") }
)
$allSpecifications = @($specifications) + @($testSpecifications)

$generatedIncludeRoot = Join-Path $OutputDirectory "Fallout4HDR"
$generatedBinaryRoot = Join-Path $generatedIncludeRoot "shaders"
$generatedTestBinaryRoot = Join-Path $generatedIncludeRoot "test-shaders"
$fxcVersion = (Get-Item -LiteralPath $fxc).VersionInfo.FileVersion

function Write-ShaderDependencyInfo {
    $outputs = @(
        (Join-Path $generatedIncludeRoot "ShippingShaders.generated.hpp")
        (Join-Path $generatedIncludeRoot "TestShaders.generated.hpp")
        (Join-Path $generatedBinaryRoot "manifest.json")
        (Join-Path $generatedTestBinaryRoot "manifest.json")
    )
    foreach ($specification in $allSpecifications) {
        $binaryRoot = if ((Test-ShaderSpecificationTestOnly $specification)) {
            $generatedTestBinaryRoot
        } else {
            $generatedBinaryRoot
        }
        $outputs += Join-Path $binaryRoot ($specification.Name + ".cso")
        $outputs += Join-Path $binaryRoot (
            $specification.Name + ".reflection.txt")
    }
    [ordered]@{
        compilerPath = $fxc
        compilerVersion = $fxcVersion
        outputs = @($outputs)
    } | ConvertTo-Json -Depth 3 -Compress
}

if ($PrintDependencyInfo) {
    Write-ShaderDependencyInfo
    return
}

$temporaryRoot = Join-Path $buildRoot (
    ".shader-generation-" + [Guid]::NewGuid().ToString("N"))
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$normalizedOutputDirectory = $OutputDirectory.Replace('/', '\').
    TrimEnd([IO.Path]::DirectorySeparatorChar).
    ToUpperInvariant()
$outputDirectoryHash = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData(
        $utf8.GetBytes($normalizedOutputDirectory)))
$generationMutex = [Threading.Mutex]::new(
    $false, "Local\Fallout4HDR.ShaderGeneration.$outputDirectoryHash")
$generationMutexOwned = $false

function Publish-CompiledShader {
    param([System.Collections.IDictionary]$Specification, [string]$SourcePath, [string]$TemporaryCso, [string]$ReflectionText)
    $bytecode = [IO.File]::ReadAllBytes($temporaryCso)
    $sourceHash = (Get-FileHash -LiteralPath $sourcePath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $bytecodeHash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($bytecode)
    ).ToLowerInvariant()
    $reflectionBytes = $utf8.GetBytes($reflectionText)
    $reflectionHash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($reflectionBytes)
    ).ToLowerInvariant()

    $binaryRoot = if ((Test-ShaderSpecificationTestOnly $specification)) {
        $generatedTestBinaryRoot
    } else {
        $generatedBinaryRoot
    }
    Write-IfChanged -Path (Join-Path $binaryRoot (
        $specification.Name + ".cso")) -Bytes $bytecode
    Write-IfChanged -Path (Join-Path $binaryRoot (
        $specification.Name + ".reflection.txt")) -Bytes $reflectionBytes

    return [pscustomobject]@{
        Specification = $specification
        Bytes = $bytecode
        SourceSha256 = $sourceHash
        BytecodeSha256 = $bytecodeHash
        ReflectionSha256 = $reflectionHash
    }
}

function Add-ShaderBytecodeDeclaration {
    param([Text.StringBuilder]$Header, [psobject]$Item)
    $symbol = $item.Specification.Symbol
    [void]$header.AppendLine(
        "inline constexpr std::array<std::byte, $($item.Bytes.Length)> $symbol = {")
    for ($offset = 0; $offset -lt $item.Bytes.Length; $offset += 12) {
        $count = [Math]::Min(12, $item.Bytes.Length - $offset)
        $values = for ($index = 0; $index -lt $count; ++$index) {
            "std::byte{0x$($item.Bytes[$offset + $index].ToString('X2'))}"
        }
        [void]$header.AppendLine("    " + ($values -join ", ") + ",")
    }
    [void]$header.AppendLine("};")
    [void]$header.AppendLine()
}

function Assert-ShaderReflection {
    param([System.Collections.IDictionary]$Specification, [string]$ReflectionText)
    if ($reflectionText -notmatch [regex]::Escape($specification.Profile)) {
        throw (
            "Reflection for $($specification.Source) does not identify " +
            "$($specification.Profile)."
        )
    }
    if ($specification.Contains("RequiredReflectionTokens")) {
        Assert-RequiredShaderTokens $specification $reflectionText
    }
    if ($specification.Name -eq "CreativeGradeLut" -and
        $reflectionText -match '(?m)^\s*dcl_resource_\S+.*\bt8\b') {
        throw "Reflection for CreativeGradeLut declares forbidden resource t8."
    }

}

function Invoke-ShaderCompilation {
    param([System.Collections.IDictionary]$Specification)
    $sourcePath = Join-Path $repoRoot $specification.Source
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Shader source does not exist: $($specification.Source)"
    }

    $temporaryCso = Join-Path $temporaryRoot (
        $specification.Name + ".cso")
    $arguments = @(
        "/nologo", "/T", $specification.Profile, "/E", "main",
        "/WX", "/Ges", "/O3", "/Zpc", "/Qstrip_debug",
        "/Fo", $temporaryCso, $sourcePath
    )
    $compilerOutput = @(& $fxc @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw (
            "FXC failed for $($specification.Source):`n" +
            ($compilerOutput -join "`n")
        )
    }
    if (-not (Test-Path -LiteralPath $temporaryCso -PathType Leaf)) {
        throw "FXC produced no bytecode for $($specification.Source)."
    }

    $reflection = @(& $fxc /dumpbin /nologo $temporaryCso 2>&1)
    if ($LASTEXITCODE -ne 0 -or $reflection.Count -eq 0) {
        throw "FXC reflection failed for $($specification.Source)."
    }
    $reflectionText = (($reflection -join "`n").Replace("`r", "")) + "`n"
    Assert-ShaderReflection $specification $reflectionText
    return Publish-CompiledShader $specification $sourcePath $temporaryCso $reflectionText
}

function Get-ShaderManifestEntry {
    param([psobject]$Item, [switch]$TestOnly)
    $entry = [ordered]@{
        name = $Item.Specification.Name
        symbol = $Item.Specification.Symbol
        source = $Item.Specification.Source
        entryPoint = "main"
        profile = $Item.Specification.Profile
    }
    if (-not $TestOnly) { $entry.defines = @() }
    $entry.sourceSha256 = $Item.SourceSha256
    $entry.bytecodeSha256 = $Item.BytecodeSha256
    $entry.reflectionSha256 = $Item.ReflectionSha256
    $entry.byteCount = $Item.Bytes.Length
    return $entry
}

function Write-ShaderManifest {
    param([object[]]$Compiled, [switch]$TestOnly)
    $entries = foreach ($item in $Compiled) {
        $isTest = (Test-ShaderSpecificationTestOnly $item.Specification)
        if ($isTest -eq [bool]$TestOnly) { Get-ShaderManifestEntry $item -TestOnly:$TestOnly }
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        windowsSdkVersion = $expectedSdk
    }
    if (-not $TestOnly) { $manifest.fxcFileVersion = $fxcVersion }
    $manifest.compilerFlags = @("/WX", "/Ges", "/O3", "/Zpc", "/Qstrip_debug")
    $manifest.shaders = @($entries)
    $text = (($manifest | ConvertTo-Json -Depth 8).Replace("`r", "")) + "`n"
    $root = if ($TestOnly) { $generatedTestBinaryRoot } else { $generatedBinaryRoot }
    Write-IfChanged -Path (Join-Path $root "manifest.json") -Bytes $utf8.GetBytes($text)
}
function Write-IfChanged {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $existing = [IO.File]::ReadAllBytes($Path)
        if ([Collections.StructuralComparisons]::StructuralEqualityComparer.Equals(
            $existing,
            $Bytes
        )) {
            return
        }
    }
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $staging = $Path + "." + [Guid]::NewGuid().ToString("N") + ".tmp"
    try {
        [IO.File]::WriteAllBytes($staging, $Bytes)
        [IO.File]::Move($staging, $Path, $true)
    } finally {
        if (Test-Path -LiteralPath $staging) {
            Remove-Item -LiteralPath $staging -Force
        }
    }
}

function Write-ShaderHeader {
    param(
        [Parameter(Mandatory = $true)][object[]]$Items,
        [Parameter(Mandatory = $true)][string]$Namespace,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $header = [Text.StringBuilder]::new()
    [void]$header.AppendLine("// Generated by scripts/compile-shaders.ps1. Do not edit.")
    [void]$header.AppendLine("#pragma once")
    [void]$header.AppendLine()
    [void]$header.AppendLine("#include <array>")
    [void]$header.AppendLine("#include <cstddef>")
    [void]$header.AppendLine()
    [void]$header.AppendLine("namespace $Namespace")
    [void]$header.AppendLine("{")
    foreach ($item in $Items) {
        Add-ShaderBytecodeDeclaration $header $item
    }
    [void]$header.AppendLine("}  // namespace $Namespace")
    $headerText = $header.ToString().Replace("`r", "")
    Write-IfChanged -Path $OutputPath -Bytes $utf8.GetBytes($headerText)
}

try {
    # Separate XMake invocations can share this generated output.
    try {
        $generationMutexOwned = $generationMutex.WaitOne(
            [TimeSpan]::FromMinutes(2))
    } catch [Threading.AbandonedMutexException] {
        $generationMutexOwned = $true
    }
    if (-not $generationMutexOwned) {
        throw (
            "Timed out waiting for shipping shader generation under " +
            "$OutputDirectory."
        )
    }

    New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $generatedBinaryRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $generatedTestBinaryRoot -Force |
        Out-Null

    $compiled = [Collections.Generic.List[object]]::new()
    foreach ($specification in $allSpecifications) {
        $compiled.Add((Invoke-ShaderCompilation $specification))
    }

    $shippingItems = @($compiled | Where-Object {
        -not ((Test-ShaderSpecificationTestOnly $_.Specification))
    })
    Write-ShaderHeader -Items $shippingItems `
        -Namespace "fo4hdr::generated" `
        -OutputPath (Join-Path $generatedIncludeRoot `
            "ShippingShaders.generated.hpp")

    $testItems = @($compiled | Where-Object {
        (Test-ShaderSpecificationTestOnly $_.Specification)
    })
    Write-ShaderHeader -Items $testItems `
        -Namespace "fo4hdr::generated::test" `
        -OutputPath (Join-Path $generatedIncludeRoot `
            "TestShaders.generated.hpp")

    Write-ShaderManifest $compiled.ToArray()
    Write-ShaderManifest $compiled.ToArray() -TestOnly
    Write-Host (
        "Compiled {0} shipping and {1} test SM5 shaders with Windows SDK {2}." -f
        $specifications.Count,
        $testSpecifications.Count,
        $expectedSdk
    )
} finally {
    try {
        if ($generationMutexOwned -and
            (Test-Path -LiteralPath $temporaryRoot)) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
    } finally {
        try {
            if ($generationMutexOwned) {
                $generationMutex.ReleaseMutex()
            }
        } finally {
            $generationMutex.Dispose()
        }
    }
}
