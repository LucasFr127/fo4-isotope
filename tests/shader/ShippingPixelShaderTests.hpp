// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "d3d11/GpuHarness.hpp"
#include <string_view>

namespace fo4hdr::tests::gpu {

void VerifyVideoPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler);

void VerifyBloomStrengthPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler);

void VerifyFxaaWideGamutPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sampler);

void VerifyCreativeGradeLutPixelShader(
    Device& device,
    ID3D11VertexShader* vertexShader,
    ID3D11SamplerState* sceneSampler);

void VerifyDeveloperExposureLock(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sampler);

void VerifyHdrGradeTransfer(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sceneSampler);

void VerifyImageControls(Device &device, ID3D11VertexShader *vertexShader, ID3D11SamplerState *sceneSampler);

void VerifyRadialVanillaGradeRemoval(Device &device, ID3D11VertexShader *vertexShader, ID3D11SamplerState *sceneSampler);

void VerifyLutProfileGradeRemoval(Device &device, ID3D11VertexShader *vertexShader, ID3D11SamplerState *sceneSampler);

void VerifyNativeGradeColorChain(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sceneSampler);

void VerifyNativeReferenceGrade(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sceneSampler);

struct TemporalVariant
{
    ID3D11VertexShader* vertexShader;
    ID3D11SamplerState* sampler;
    ID3D11PixelShader* shader;
    bool secondVariant;
    std::string_view variantName;
};

void VerifyTemporalWideGamutPixelShader(Device& device, TemporalVariant variant);

}  // namespace fo4hdr::tests::gpu
