// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace fo4hdr {

struct PixelBindingRequest
{
    UINT constantBufferSlot{};
    ID3D11Buffer* constantBuffer{};
    UINT shaderResourceSlot{};
    ID3D11ShaderResourceView* shaderResource{};
    UINT samplerSlot{};
    ID3D11SamplerState* sampler{};
    bool bindShaderResource{};
    UINT extraShaderResourceSlot{};
    ID3D11ShaderResourceView* extraShaderResource{};
    bool rejectExtraShaderResourceCollision{true};
    bool rejectShaderResourceCollision{};

    [[nodiscard]] bool HasExtraResource() const noexcept
    {
        return extraShaderResource != nullptr;
    }
    [[nodiscard]] bool PrimaryValid() const noexcept
    {
        return !bindShaderResource || (shaderResource != nullptr && sampler != nullptr);
    }
    [[nodiscard]] bool ExtraValid() const noexcept
    {
        if (!HasExtraResource()) return true;
        const auto overlapsPrimary = bindShaderResource && extraShaderResourceSlot == shaderResourceSlot;
        return !overlapsPrimary && extraShaderResourceSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    }
    [[nodiscard]] bool Valid() const noexcept
    {
        return constantBuffer != nullptr && PrimaryValid() && ExtraValid();
    }
};

// Both context interfaces are borrowed for the draw scope. The caller supplies
// the same generation's cached Context1 so state capture needs no interface query.
class ScopedD3D11PixelBindings
{
public:
    ScopedD3D11PixelBindings() noexcept = default;

    ~ScopedD3D11PixelBindings() noexcept;

    ScopedD3D11PixelBindings(const ScopedD3D11PixelBindings&) = delete;
    ScopedD3D11PixelBindings& operator=(
        const ScopedD3D11PixelBindings&) = delete;

    [[nodiscard]] bool Bind(ID3D11DeviceContext* context,
        ID3D11DeviceContext1* context1, const PixelBindingRequest& request) noexcept;

    void Reset() noexcept;

    [[nodiscard]] bool Active() const noexcept { return active_; }

private:
    static bool ForeignResource(ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* requested, bool rejectCollision) noexcept;

    struct ResourceBorrow
    {
        UINT slot;
        ID3D11ShaderResourceView* resource;
        bool rejectCollision;
    };

    static bool CaptureResource(ID3D11DeviceContext* context, ResourceBorrow request,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& previous) noexcept;

    bool CaptureResources(ID3D11DeviceContext* context, const PixelBindingRequest& request) noexcept;

    void PrepareContext(ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
        const PixelBindingRequest& request) noexcept;

    void ApplyBindings(const PixelBindingRequest& request) noexcept;

    ID3D11DeviceContext* context_{};
    ID3D11DeviceContext1* context1_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> previousConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousShaderResource_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousExtraShaderResource_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler_;
    UINT constantBufferSlot_{};
    UINT previousFirstConstant_{};
    UINT previousNumConstants_{};
    UINT shaderResourceSlot_{};
    UINT extraShaderResourceSlot_{};
    UINT samplerSlot_{};
    bool bindShaderResource_{};
    bool bindExtraShaderResource_{};
    bool active_{};
};

}  // namespace fo4hdr
