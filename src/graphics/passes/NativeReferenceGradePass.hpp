// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "../resources/NativeReferenceResources.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>

namespace fo4hdr {

struct NativeReferenceInputs
{
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> looks;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    UINT firstConstant{};
    UINT constantCount{};
};

[[nodiscard]] bool IsQualifiedNativeReferenceLook(ID3D11ShaderResourceView* view) noexcept;

[[nodiscard]] bool CaptureNativeReferenceInputs(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, NativeReferenceInputs& inputs) noexcept;

void DispatchNativeReferencePrepare(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, const NativeReferenceInputs& inputs,
    const NativeReferenceResources& state) noexcept;

} // namespace fo4hdr
