// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "../OperatorConstants.hpp"
#include "../OperatorLutWorker.hpp"
#include "../RenoDxShaderIdentity.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include "NativeReferenceResources.hpp"
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
    NativeReferenceResources nativeReference;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> temporalAa;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> temporalAa2;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> fxaa;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> video;
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
extern const char* g_nativeReferenceStatus;
bool ReplacementProgramsReady() noexcept;
bool ReplacementConstantBuffersReady() noexcept;
bool ReplacementSetMatchesConfiguration() noexcept;
ID3D11PixelShader* FindReplacement(RenoDxShaderPass) noexcept;
void ResetReplacementShaders() noexcept;
bool BuildReplacementShaders(ID3D11Device*) noexcept;
bool PublishReplacementSettings(ID3D11DeviceContext*, const HdrSettings&, std::uint64_t generation) noexcept;
bool PublishReplacementLut(ID3D11Device*, ID3D11DeviceContext*,
    const OperatorLutBuildResult&, const HdrSettings&) noexcept;

} // namespace fo4hdr
