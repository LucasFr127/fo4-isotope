// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "BorrowedComputeState.hpp"

namespace fo4hdr {

// Callers reject query scopes affected by owned raster or compute work.
class BorrowedFullscreenState
{
public:
    BorrowedFullscreenState(ID3D11DeviceContext* context,
        ID3D11DeviceContext1* context1, ID3D11Resource* destination) noexcept;
    ~BorrowedFullscreenState() noexcept;
    BorrowedFullscreenState(const BorrowedFullscreenState&) = delete;
    BorrowedFullscreenState& operator=(const BorrowedFullscreenState&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return context_ != nullptr; }

private:
    template <class Shader> struct ShaderState
    {
        Microsoft::WRL::ComPtr<Shader> shader;
        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> classes{};
        UINT count{D3D11_SHADER_MAX_INTERFACES};

        template <class Getter>
        void Capture(ID3D11DeviceContext* context, Getter getter) noexcept
        {
            (context->*getter)(&shader, classes.data(), &count);
        }
        template <class Setter>
        void Restore(ID3D11DeviceContext* context, Setter setter) noexcept
        {
            (context->*setter)(shader.Get(), classes.data(), count);
            for (auto* instance : classes) if (instance) instance->Release();
        }
    };

    void CaptureGraphics() noexcept;
    void RestoreGraphics() noexcept;

    ID3D11DeviceContext* context_{};
    ID3D11DeviceContext1* context1_{};
    BorrowedComputeState outputs_;
    ShaderState<ID3D11VertexShader> vertex_;
    ShaderState<ID3D11HullShader> hull_;
    ShaderState<ID3D11DomainShader> domain_;
    ShaderState<ID3D11GeometryShader> geometry_;
    ShaderState<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_{};
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors_{};
    UINT viewportCount_{static_cast<UINT>(viewports_.size())};
    UINT scissorCount_{static_cast<UINT>(scissors_.size())};
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    std::array<FLOAT, 4> blendFactor_{};
    UINT sampleMask_{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    UINT stencilReference_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    UINT firstConstant_{}, constantCount_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexConstants_;
    UINT firstVertexConstant_{}, vertexConstantCount_{};
    std::array<ID3D11ShaderResourceView*, 4> resources_{};
};

} // namespace fo4hdr
