// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "../resources/AffineResources.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>

namespace fo4hdr {

struct AffineInputs
{
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> looks;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    UINT firstConstant{};
    UINT constantCount{};
};

[[nodiscard]] bool IsQualifiedAffineLook(ID3D11ShaderResourceView* view) noexcept;

[[nodiscard]] bool CaptureAffineInputs(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, AffineInputs& inputs) noexcept;

void DispatchAffinePrepare(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, const AffineInputs& inputs,
    const AffineResources& state) noexcept;

} // namespace fo4hdr
