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
        std::span<const std::byte> captureBytecode,
        float sceneExposure = 1.0F, float bloom = 1.0F) noexcept;
    // Cold, closed-frame update: publish both selector variants together or keep both.
    [[nodiscard]] bool SetControls(ID3D11Device* device, float sceneExposure, float bloom) noexcept;
    // Full retirement only. Policy invalidation must retain the disabled selector.
    void Reset() noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] ID3D11Buffer* Selector(bool enabled) const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* Snapshot() const noexcept;
    // Requalify the current tone inputs without overwriting the captured pair.
    [[nodiscard]] bool CanReplay(ID3D11DeviceContext* context) const noexcept;

    // The caller excludes queries, command lists and snapshot aliases. This submits one capture
    // and restores touched CS state and exact b2 range before returning; policy stays inactive.
    [[nodiscard]] bool TryCapture(ID3D11DeviceContext* context) noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> selectors_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> target_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
};

// The tone draw borrows b13/t17 and restores their actual object/range tuple.
class ScopedDeveloperExposureBindings final
{
public:
    ScopedDeveloperExposureBindings(ID3D11DeviceContext1* context, bool tone) noexcept;
    ~ScopedDeveloperExposureBindings() noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    void Bind(ID3D11Buffer* selector, ID3D11ShaderResourceView* snapshot) noexcept;
    ScopedDeveloperExposureBindings(const ScopedDeveloperExposureBindings&) = delete;
    ScopedDeveloperExposureBindings& operator=(const ScopedDeveloperExposureBindings&) = delete;
private:
    ID3D11DeviceContext1* context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> previousConstants_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousSource_;
    UINT firstConstant_{};
    UINT constantCount_{};
    bool tone_{};
    bool bound_{};
};

}  // namespace fo4hdr
