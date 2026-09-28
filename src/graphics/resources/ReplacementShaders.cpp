// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "ReplacementShaders.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "PresentationResources.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../RendererLifetime.hpp"
#include "../LiveSettings.hpp"
#include <FO4Isotope/ShippingShaders.generated.hpp>
#include <algorithm>
#include <span>
#include <cstddef>
#include <cstring>
#include <utility>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

ReplacementShaderSet g_replacements;
const char* g_nativeReferenceStatus = "Waiting for a color grading draw.";

struct ReplacementProgram
{
    RenoDxShaderPass pass;
    std::span<const std::byte> bytecode;
    ComPtr<ID3D11PixelShader> ReplacementShaderSet::*destination;
};

constexpr ReplacementProgram kReplacementPrograms[]{
    {RenoDxShaderPass::toneMapA, generated::kToneMapA, &ReplacementShaderSet::toneMapA},
    {RenoDxShaderPass::toneMapB, generated::kToneMapB, &ReplacementShaderSet::toneMapB},
    {RenoDxShaderPass::toneMapNewA, generated::kToneMapNewA, &ReplacementShaderSet::toneMapNewA},
    {RenoDxShaderPass::toneMapNewB, generated::kToneMapNewB, &ReplacementShaderSet::toneMapNewB},
    {RenoDxShaderPass::lut, generated::kCreativeGradeLut, &ReplacementShaderSet::lut},
    {RenoDxShaderPass::taa, generated::kTemporalAa, &ReplacementShaderSet::temporalAa},
    {RenoDxShaderPass::taa2, generated::kTemporalAa2, &ReplacementShaderSet::temporalAa2},
    {RenoDxShaderPass::fxaa, generated::kFxaa, &ReplacementShaderSet::fxaa},
    {RenoDxShaderPass::video, generated::kVideo, &ReplacementShaderSet::video}
};

[[nodiscard]] ID3D11PixelShader* FindReplacement(RenoDxShaderPass pass) noexcept
{
    const auto resourcesReady = IsWideGamutGenerationReady(
        CurrentWideGamutReadiness(), g_presentation.colorDomainGeneration,
        g_evidence.resourceGeneration.load(std::memory_order_acquire));
    if (!resourcesReady) return nullptr;
    const auto program = std::ranges::find(kReplacementPrograms, pass, &ReplacementProgram::pass);
    if (program == std::end(kReplacementPrograms)) return nullptr;
    return (g_replacements.*program->destination).Get();
}

void ResetReplacementShaders() noexcept
{
    g_replacements.configurationReady = false;
    g_replacements.configurationGeneration = 0;
    g_nativeReferenceStatus = "Waiting for a color grading draw.";
    g_replacements.toneMapA.Reset();
    g_replacements.toneMapB.Reset();
    g_replacements.toneMapNewA.Reset();
    g_replacements.toneMapNewB.Reset();
    g_replacements.lut.Reset();
    ResetNativeReferenceResources(g_replacements.nativeReference);
    g_replacements.temporalAa.Reset();
    g_replacements.temporalAa2.Reset();
    g_replacements.fxaa.Reset();
    g_replacements.video.Reset();
    g_replacements.bypassConstants.Reset();
    g_replacements.operatorConstants.Reset();
    g_replacements.lutProfileConstants.Reset();
    g_replacements.lutProfileKeyReady = false;
    g_replacements.operatorLutSampler.Reset();
    g_replacements.operatorLutSrv.Reset();
    g_replacements.operatorLut.Reset();
    g_replacements.operatorLutKey = {};
    g_replacements.configurationKey = {};
    g_replacements.device = nullptr;
}

[[nodiscard]] bool UploadOperatorLut(
    ID3D11Device* device,
    const OperatorLutData& lut,
    ComPtr<ID3D11Texture3D>& texture,
    ComPtr<ID3D11ShaderResourceView>& srv) noexcept
{
    if (device == nullptr || !lut.IsValid()) return false;
    D3D11_TEXTURE3D_DESC textureDescription{};
    textureDescription.Width = lut.textureEdge;
    textureDescription.Height = lut.textureEdge;
    textureDescription.Depth = lut.textureEdge;
    textureDescription.MipLevels = 1;
    textureDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA textureData{
        lut.rgba16.data(),
        static_cast<UINT>(lut.textureEdge * 4U * sizeof(std::uint16_t)),
        static_cast<UINT>(lut.textureEdge * lut.textureEdge * 4U *
            sizeof(std::uint16_t))
    };
    if (FAILED(device->CreateTexture3D(&textureDescription,
            &textureData, texture.GetAddressOf())))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = textureDescription.Format;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    viewDescription.Texture3D.MostDetailedMip = 0;
    viewDescription.Texture3D.MipLevels = 1;
    return SUCCEEDED(device->CreateShaderResourceView(texture.Get(),
        &viewDescription, srv.GetAddressOf()));
}

[[nodiscard]] bool PublishOperatorConstants(
    ID3D11DeviceContext* context,
    const HdrSettings& values,
    OperatorConstants& published) noexcept
{
    if (context == nullptr || !g_replacements.operatorConstants)
        return false;
    if (!BuildOperatorSettings(values, published))
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_replacements.operatorConstants.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    g_replacements.configurationReady = false;
    g_replacements.configurationGeneration = 0;
    std::memcpy(mapped.pData, &published, sizeof(published));
    context->Unmap(g_replacements.operatorConstants.Get(), 0);
    return true;
}

void PublishReplacementConfiguration(
    const OperatorConstants& constants, std::uint64_t generation) noexcept
{
    g_replacements.configurationKey = constants;
    g_replacements.configurationGeneration = generation;
    g_replacements.configurationReady = true;
}

[[nodiscard]] bool PublishReplacementSettings(ID3D11DeviceContext* context,
    const HdrSettings& settings, std::uint64_t generation) noexcept
{
    OperatorConstants constants{};
    if (!PublishOperatorConstants(context, settings, constants)) return false;
    PublishReplacementConfiguration(constants, generation);
    return true;
}

[[nodiscard]] bool PublishReplacementLut(ID3D11Device* device,
    ID3D11DeviceContext* context, const OperatorLutBuildResult& result,
    const HdrSettings& settings) noexcept
{
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    OperatorConstants constants{};
    if (!UploadOperatorLut(device, result.data, texture, srv) ||
        !PublishOperatorConstants(context, settings, constants))
        return false;
    g_replacements.operatorLut = std::move(texture);
    g_replacements.operatorLutSrv = std::move(srv);
    g_replacements.operatorLutKey = result.request;
    g_replacements.operatorLutKey.deviceGeneration = g_deviceGeneration;
    // The LUT cannot recover constant-only edits whose earlier Map
    // failed; retain their unpublished generation and error status.
    PublishReplacementConfiguration(constants, g_settingsGeneration);
    return true;
}

[[nodiscard]] bool CreateReplacementPrograms(ID3D11Device* device,
    ReplacementShaderSet& candidate) noexcept
{
    for (const auto& program : kReplacementPrograms)
    {
        if (program.bytecode.empty()) return false;
        if (FAILED(device->CreatePixelShader(program.bytecode.data(),
                program.bytecode.size(), nullptr,
                (candidate.*program.destination).GetAddressOf())))
            return false;
    }
    return true;
}

[[nodiscard]] bool CreateReplacementConstants(ID3D11Device* device,
    ReplacementShaderSet& candidate, const OperatorConstants& settings) noexcept
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(settings);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    OperatorConstants bypass = settings;
    bypass.mode = 0U;
    bypass.uiTransportWhiteNits =
        static_cast<float>(kSceneReferenceWhiteNits);
    bypass.sceneExposureMultiplier = 1.0F;
    bypass.bloomStrength = 1.0F;
    bypass.vanillaColorGradingStrength = 1.0F;
    bypass.contrastSlope = 1.0F;
    bypass.highlightsEv = 0.0F;
    bypass.shadowsEv = 0.0F;
    bypass.saturationScale = 1.0F;
    bypass.liveSettingsReserved = 0U;
    bypass.blacksStrength = 0.0F;
    bypass.liveSettingsReserved1 = 0U;
    bypass.colorBalanceRow0 = {1.0F, 0.0F, 0.0F, 0.0F};
    bypass.colorBalanceRow1 = {0.0F, 1.0F, 0.0F, 0.0F};
    bypass.colorBalanceRow2 = {0.0F, 0.0F, 1.0F, 0.0F};
    bypass.selectiveColorActive = 0U;
    bypass.selectiveColorPlaneA[3] = 0.0F;
    bypass.selectiveColorPlaneB[3] = 0.0F;
    D3D11_SUBRESOURCE_DATA bypassData{&bypass, 0, 0};
    if (FAILED(device->CreateBuffer(&description, &bypassData,
            &candidate.bypassConstants)))
        return false;

    description.Usage = D3D11_USAGE_DYNAMIC;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA operatorData{&settings, 0, 0};
    if (FAILED(device->CreateBuffer(&description, &operatorData,
            &candidate.operatorConstants)))
        return false;

    // Profile failure leaves the existing global grade controls available.
    (void)device->CreateBuffer(&description, &operatorData,
        &candidate.lutProfileConstants);

    return true;
}

[[nodiscard]] bool CreateReplacementLut(ID3D11Device* device,
    ReplacementShaderSet& candidate) noexcept
{
    // The CPU cache survives device loss; only GPU resources are recreated.
    if (!g_activeCpuLut) return false;
    if (!PreparedOperatorLutMatches(g_activeCpuLut->data, {
            .algorithm = g_activeSettings.productionAlgorithm,
            .displayPeakNits = g_activeSettings.displayPeakNits,
            .toneMapperParameters = g_activeSettings.toneMapperParameters}))
        return false;
    if (!UploadOperatorLut(device, g_activeCpuLut->data,
            candidate.operatorLut, candidate.operatorLutSrv))
        return false;
    candidate.operatorLutKey = {
        .algorithm = g_activeSettings.productionAlgorithm,
        .displayPeakNits = g_activeSettings.displayPeakNits,
        .toneMapperParameters = ActiveToneMapperParameters(
            g_activeSettings.productionAlgorithm,
            g_activeSettings.toneMapperParameters)
    };
    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDescription.MinLOD = 0.0F;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDescription,
            &candidate.operatorLutSampler)))
        return false;
    return true;
}

[[nodiscard]] bool BuildReplacementShaders(ID3D11Device* device) noexcept
{
    if (device == nullptr) return false;
    ReplacementShaderSet candidate;
    if (!CreateReplacementPrograms(device, candidate)) return false;
    // A failed optional grading allocation keeps the Radial renderer available.
    (void)BuildNativeReferenceResources(device, candidate.nativeReference);
    OperatorConstants settings{};
    if (!BuildOperatorSettings(settings)) return false;
    if (!CreateReplacementConstants(device, candidate, settings)) return false;
    if (RequiresOperatorLut(settings.mode) &&
        !CreateReplacementLut(device, candidate))
        return false;
    candidate.configurationKey = settings;
    candidate.configurationGeneration = g_settingsGeneration;
    candidate.configurationReady = true;
    candidate.device = device;
    g_replacements = std::move(candidate);
    return true;
}

[[nodiscard]] bool ReplacementLutMatchesConfiguration() noexcept
{
    return g_replacements.operatorLut &&
             g_replacements.operatorLutSrv &&
             g_replacements.operatorLutSampler &&
             g_replacements.operatorLutKey.algorithm ==
                g_activeSettings.productionAlgorithm &&
             g_replacements.operatorLutKey.displayPeakNits ==
                g_activeSettings.displayPeakNits &&
             g_replacements.operatorLutKey.toneMapperParameters ==
                ActiveToneMapperParameters(
                    g_activeSettings.productionAlgorithm,
                    g_activeSettings.toneMapperParameters);
}

[[nodiscard]] bool ReplacementSetMatchesConfiguration() noexcept
{
    return g_replacements.configurationReady &&
        g_replacements.configurationGeneration == g_settingsGeneration &&
        (!RequiresOperatorLut(g_replacements.configurationKey.mode) ||
            ReplacementLutMatchesConfiguration());
}


[[nodiscard]] bool ToneMapProgramsReady() noexcept
{
    const std::array programs{g_replacements.toneMapA.Get(), g_replacements.toneMapB.Get(),
        g_replacements.toneMapNewA.Get(), g_replacements.toneMapNewB.Get()};
    return std::ranges::find(programs, nullptr) == programs.end();
}

[[nodiscard]] bool PostProcessProgramsReady() noexcept
{
    const std::array programs{g_replacements.lut.Get(), g_replacements.temporalAa.Get(),
        g_replacements.temporalAa2.Get(), g_replacements.fxaa.Get(), g_replacements.video.Get()};
    return std::ranges::find(programs, nullptr) == programs.end();
}

[[nodiscard]] bool ReplacementConstantBuffersReady() noexcept
{
    return g_replacements.bypassConstants && g_replacements.operatorConstants;
}

[[nodiscard]] bool ReplacementConstantsReady() noexcept
{
    return ReplacementConstantBuffersReady() &&
        g_replacements.device && ReplacementSetMatchesConfiguration();
}

[[nodiscard]] bool ReplacementProgramsReady() noexcept
{
    return ToneMapProgramsReady() && PostProcessProgramsReady() && ReplacementConstantsReady();
}


} // namespace fo4hdr
