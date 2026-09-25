// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <d3d11_1.h>

namespace fo4hdr {

using ArmorEncodedSetRenderTargets = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

struct ArmorEncodedDispatch
{
    ID3D11ComputeShader* shader;
    ID3D11UnorderedAccessView* output;
    ID3D11Buffer* constants;
    UINT width;
    UINT height;

    [[nodiscard]] bool Ready() const noexcept
    {
        return shader && output && constants && width && height;
    }
};

[[nodiscard]] bool DispatchArmorEncodedComposition(
    ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
    const ArmorEncodedDispatch& dispatch,
    ArmorEncodedSetRenderTargets setRenderTargets = nullptr) noexcept;

} // namespace fo4hdr
