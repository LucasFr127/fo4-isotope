// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace fo4hdr {

// Private views never escape the renderer or remain bound after preparation.
struct NativeReferenceResources
{
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> preblendShader;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> preblendTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preblendSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> preblendUav;
};

[[nodiscard]] bool BuildNativeReferenceResources(ID3D11Device* device,
    NativeReferenceResources& resources) noexcept;

void ResetNativeReferenceResources(NativeReferenceResources& resources) noexcept;

[[nodiscard]] bool NativeReferenceResourcesReady(const NativeReferenceResources& resources) noexcept;

} // namespace fo4hdr
