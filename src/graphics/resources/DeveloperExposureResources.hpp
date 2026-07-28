// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <span>

namespace fo4hdr {

// Cold-owned resources on one exclusively owned immediate context. The owner
// alone binds Snapshot, and clears its aliases before cold retirement.
class DeveloperExposureResources final
{
public:
    // Success means the mandatory disabled tone selector exists. Optional
    // capture failure retains that selector; Ready reports the complete bundle,
    // restricted to FL11_0 with checked constant-buffer offsetting support.
    [[nodiscard]] bool Prepare(ID3D11Device* device,
        std::span<const std::byte> captureBytecode) noexcept;
    // Full retirement only. Policy invalidation must retain the disabled selector.
    void Reset() noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] ID3D11Buffer* Selector(bool enabled) const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* Snapshot() const noexcept;
    // Requalify the current tone inputs without overwriting the captured pair.
    [[nodiscard]] bool CanReplay(ID3D11DeviceContext* context) const noexcept;

    // Only an idle CS/predicate/UAV context qualifies. This submits one capture
    // and clears its own bindings before returning; it does not activate policy.
    [[nodiscard]] bool TryCapture(ID3D11DeviceContext* context) noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> selectors_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> target_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
};

}  // namespace fo4hdr
