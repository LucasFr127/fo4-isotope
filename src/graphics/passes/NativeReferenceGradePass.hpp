// SPDX-License-Identifier: GPL-3.0-or-later
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

// Internal caller keeps the immediate context exclusive and supplies qualified
// look/weight semantics; getters prove layout and binding, not native identity.
[[nodiscard]] bool IsQualifiedNativeReferenceLook(ID3D11ShaderResourceView* view) noexcept;

[[nodiscard]] bool CaptureNativeReferenceInputs(ID3D11DeviceContext1* context, NativeReferenceInputs& inputs) noexcept;

[[nodiscard]] bool DispatchNativeReferencePrepare(ID3D11DeviceContext1* context, const NativeReferenceInputs& inputs,
    const NativeReferenceResources& state) noexcept;

} // namespace fo4hdr
