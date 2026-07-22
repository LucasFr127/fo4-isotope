// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace fo4hdr {

// Borrows only b13's exact window, t7 and s7 on the qualified owner Context1.
// A null replacement buffer leaves all three slots untouched.
class ScopedD3D11PixelBindings
{
public:
    ScopedD3D11PixelBindings(ID3D11DeviceContext1* context,
        ID3D11Buffer* constants) noexcept;
    ~ScopedD3D11PixelBindings() noexcept;
    ScopedD3D11PixelBindings(const ScopedD3D11PixelBindings&) = delete;
    ScopedD3D11PixelBindings& operator=(const ScopedD3D11PixelBindings&) = delete;

    [[nodiscard]] bool Ready() const noexcept;
    void Bind(ID3D11ShaderResourceView* source, ID3D11SamplerState* sampler) noexcept;

private:
    ID3D11DeviceContext1* context_;
    ID3D11Buffer* constants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> previousConstants_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousSource_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler_;
    UINT firstConstant_{};
    UINT constantCount_{};
    bool bound_{};
};

}  // namespace fo4hdr
