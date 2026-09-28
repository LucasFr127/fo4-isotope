// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>

namespace fo4hdr {

// Context interfaces are borrowed for the enclosing dispatch's lifetime. The
// caller supplies its generation-cached Context1; capturing state never performs QI.
class ScopedD3D11ComputeConstantBuffer
{
public:
    ScopedD3D11ComputeConstantBuffer() noexcept = default;
    ScopedD3D11ComputeConstantBuffer(const ScopedD3D11ComputeConstantBuffer&) = delete;
    ScopedD3D11ComputeConstantBuffer& operator=(
        const ScopedD3D11ComputeConstantBuffer&) = delete;
    ~ScopedD3D11ComputeConstantBuffer() noexcept;

    void Capture(ID3D11DeviceContext* context, ID3D11DeviceContext1* cachedContext1,
        UINT slot) noexcept;

    void Restore() noexcept;

private:
    ID3D11DeviceContext* context_{};
    ID3D11DeviceContext1* context1_{};
    ID3D11Buffer* buffer_{};
    UINT slot_{};
    UINT firstConstant_{};
    UINT constantCount_{};
};

} // namespace fo4hdr
