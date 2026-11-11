// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "../OperatorConstants.hpp"
#include "../OperatorLutWorker.hpp"
#include "../RenoDxShaderIdentity.hpp"
#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include "AffineResources.hpp"
#include <d3d11.h>
#include <wrl/client.h>

namespace fo4hdr {

struct ReplacementShaderSet
{
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapA;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapB;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapNewA;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapNewB;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lut;
    AffineResources affine;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> temporalAa;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> temporalAa2;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> fxaa;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> video;
    // SDR-mode set; it holds no HDR programs, and only a selected operator
    // needs the table and sampler below.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapASdr;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapBSdr;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapNewASdr;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> toneMapNewBSdr;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lutSdr;
    // Hosted-HDR set: the HDR tone maps above and a LUT that writes the host's
    // scene format, plain or contained for a target without negative values.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lutHosted;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lutHostedContained;
    // Ends SDR-mode or hosted substitution for the device generation.
    bool sdrSubstitutionDisabled{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> bypassConstants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> operatorConstants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> lutProfileConstants;
    OperatorConstants lutProfileKey{};
    bool lutProfileKeyReady{};
    Microsoft::WRL::ComPtr<ID3D11Texture3D> operatorLut;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> operatorLutSrv;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> operatorLutSampler;
    OperatorLutBuildRequest operatorLutKey{};
    OperatorConstants configurationKey{};
    std::uint64_t configurationGeneration{};
    bool configurationReady{};
    ID3D11Device* device{};
};

extern ReplacementShaderSet g_replacements;
extern const char* g_affineStatus;
bool ReplacementProgramsReady() noexcept;
bool ReplacementConstantBuffersReady() noexcept;
bool ReplacementSetMatchesConfiguration() noexcept;
ID3D11PixelShader* FindReplacement(RenoDxShaderPass) noexcept;
// Null for a neutral pass, so its original shader draws.
ID3D11PixelShader* FindSdrReplacement(RenoDxShaderPass) noexcept;
// Tone-map family and LUT only, in gameplay, while the host still reports HDR.
ID3D11PixelShader* FindHostedReplacement(RenoDxShaderPass) noexcept;
void DisableSdrSubstitution() noexcept;
void ResetReplacementShaders() noexcept;
bool BuildReplacementShaders(ID3D11Device*) noexcept;
// Requires the SDR latch, so the constants carry the SDR operator mode.
bool BuildSdrReplacementShaders(ID3D11Device*) noexcept;
// Requires the hosted-HDR latch, so the constants carry the HDR operator mode.
bool BuildHostedReplacementShaders(ID3D11Device*) noexcept;
bool PublishReplacementSettings(ID3D11DeviceContext*, const HdrSettings&, std::uint64_t generation) noexcept;
bool PublishReplacementLut(ID3D11Device*, ID3D11DeviceContext*,
    const OperatorLutBuildResult&, const HdrSettings&) noexcept;
// Drops the table once no mode reads it; the sampler stays for the next one.
void ReleaseReplacementLut() noexcept;

} // namespace fo4hdr
