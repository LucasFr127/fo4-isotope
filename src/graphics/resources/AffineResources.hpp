// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace fo4hdr {

struct AffineResources
{
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> preblendShader;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> preblendTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preblendSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> preblendUav;
};

[[nodiscard]] bool BuildAffineResources(ID3D11Device* device,
    AffineResources& resources) noexcept;

void ResetAffineResources(AffineResources& resources) noexcept;

[[nodiscard]] bool AffineResourcesReady(const AffineResources& resources) noexcept;

} // namespace fo4hdr
