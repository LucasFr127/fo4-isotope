// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace fo4hdr {

// Borrows the qualified Context1 and preserves the exact CS-b0 window.
// Capture and explicit restoration stay in the enclosing dispatch's state order.
class ScopedD3D11ComputeConstantBuffer
{
public:
    explicit ScopedD3D11ComputeConstantBuffer(ID3D11DeviceContext1* context) noexcept;
    ~ScopedD3D11ComputeConstantBuffer() noexcept;
    ScopedD3D11ComputeConstantBuffer(const ScopedD3D11ComputeConstantBuffer&) = delete;
    ScopedD3D11ComputeConstantBuffer& operator=(const ScopedD3D11ComputeConstantBuffer&) = delete;

    void Capture() noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    void Bind(ID3D11Buffer* constants) noexcept;
    void Restore() noexcept;

private:
    ID3D11DeviceContext1* context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    UINT firstConstant_{};
    UINT constantCount_{};
    bool bound_{};
};

}  // namespace fo4hdr
