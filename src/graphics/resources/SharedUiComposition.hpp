// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace fo4hdr {

struct SharedUiWhite
{
    float sceneWhiteNits{};
    float uiWhiteNits{};
};

enum class SharedUiSeedMode : std::uint32_t
{
    linearScene,
    scaleVanillaSdr
};

struct SharedUiSeedInputs
{
    ID3D11ShaderResourceView* scene{};
    ID3D11RenderTargetView* output{};
    SharedUiWhite white;
    SharedUiSeedMode mode{};
};

class SharedUiComposition
{
public:
    [[nodiscard]] bool Create(ID3D11Device*, std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] bool InputsCompatible(const SharedUiSeedInputs&) const noexcept;
    // The caller rejects active queries and owns the context observation scope.
    // A false result leaves the public output untouched.
    [[nodiscard]] bool Seed(ID3D11DeviceContext*, ID3D11DeviceContext1*,
        const SharedUiSeedInputs&) noexcept;
    void Reset() noexcept;

private:
    bool CreateTextures() noexcept;
    bool CreatePipeline() noexcept;
    bool CreateConstants() noexcept;
    bool ContextMatches(ID3D11DeviceContext*, ID3D11DeviceContext1*) const noexcept;
    bool TextureMatches(ID3D11Resource*) const noexcept;
    bool ResolveInputs(const SharedUiSeedInputs&,
        Microsoft::WRL::ComPtr<ID3D11Resource>& scene,
        Microsoft::WRL::ComPtr<ID3D11Resource>& output) const noexcept;
    bool UploadWhite(ID3D11DeviceContext*, const SharedUiSeedInputs&) noexcept;
    void Draw(ID3D11DeviceContext*, ID3D11RenderTargetView*) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scratch_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scratchSrv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    std::uint32_t width_{}, height_{};
};

} // namespace fo4hdr
