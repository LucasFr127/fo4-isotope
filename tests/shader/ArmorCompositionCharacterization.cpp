// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "d3d11/GpuHarness.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace fo4hdr::tests::gpu {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11BlendState> CreateBlend(ID3D11Device* device, bool additive)
{
    D3D11_BLEND_DESC description{};
    auto& target = description.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = additive ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> state;
    CheckHresult(device->CreateBlendState(&description, &state), "Armor fixture blend state");
    return state;
}

struct BlendFixture
{
    Device gpu{CreateWarpDevice()};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11RasterizerState> rasterizer;

    explicit BlendFixture(const std::filesystem::path& directory)
    {
        auto description = detail::SceneDescription(1, 1);
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        CheckHresult(gpu.device->CreateTexture2D(&description, nullptr, &texture), "Armor fixture FP16 target");
        CheckHresult(gpu.device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Armor fixture RTV");
        CreateShaders(directory);
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        CheckHresult(gpu.device->CreateRasterizerState(&raster, &rasterizer), "Armor fixture rasterizer");
    }

    ~BlendFixture() noexcept
    {
        // This fixture exclusively owns its context; release its bound references first.
        gpu.context->ClearState();
    }

    void CreateShaders(const std::filesystem::path& directory)
    {
        const auto vs = ReadShader(directory / "Fullscreen.dxbc");
        CheckHresult(gpu.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex), "Armor fixture vertex shader");
        const auto ps = ReadShader(directory / "ArmorCharacterization.dxbc");
        CheckHresult(gpu.device->CreatePixelShader(ps.data(), ps.size(), nullptr, &pixel), "Armor fixture pixel shader");
    }

    Float4 Blend(Float4 source, bool additive)
    {
        constexpr std::array white{1.0f, 1.0f, 1.0f, 1.0f};
        const std::array values{source};
        const auto constants = detail::ConstantBuffer(gpu.device.Get(), values);
        const auto blend = CreateBlend(gpu.device.Get(), additive);
        auto* buffer = constants.Get();
        auto* output = target.Get();
        gpu.context->ClearRenderTargetView(output, white.data());
        gpu.context->OMSetRenderTargets(1, &output, nullptr);
        gpu.context->OMSetBlendState(blend.Get(), nullptr, ~UINT{0});
        gpu.context->IASetInputLayout(nullptr);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->VSSetShader(vertex.Get(), nullptr, 0);
        gpu.context->PSSetShader(pixel.Get(), nullptr, 0);
        gpu.context->PSSetConstantBuffers(0, 1, &buffer);
        gpu.context->RSSetState(rasterizer.Get());
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        gpu.context->RSSetViewports(1, &viewport);
        gpu.context->Draw(3, 0);
        const auto outputPixels = detail::ReadScene(gpu, texture.Get());
        if (outputPixels.size() != 1) throw std::runtime_error("Armor fixture readback extent");
        return outputPixels[0];
    }
};

void CheckArithmetic(Float4 actual, float expected)
{
    const std::array channels{actual.red, actual.green, actual.blue};
    for (const auto channel : channels)
    {
        FO4HDR_CHECK(std::isfinite(channel));
        FO4HDR_CHECK(channel == expected);
    }
    FO4HDR_CHECK(actual.alpha == 1.0f);
}

void CheckInterpretation(float stored)
{
    // Native blending operates on stored values; gamma decoding is a separate interpretation.
    const auto decoded = std::pow(static_cast<double>(stored), 2.2);
    const auto encoded = std::pow(decoded, 1.0 / 2.2);
    FO4HDR_CHECK(std::isfinite(decoded));
    FO4HDR_CHECK(std::abs(encoded - stored) <= 1.0e-12);
    FO4HDR_CHECK(std::abs(decoded - stored) > 0.25);
}

}  // namespace

void VerifyArmorCompositionCharacterization(const std::filesystem::path& directory)
{
    BlendFixture fixture(directory);
    const auto halfOpacity = fixture.Blend({0, 0, 0, 0.5f}, false);
    CheckArithmetic(halfOpacity, 0.5f);
    CheckInterpretation(halfOpacity.red);
    const auto additive = fixture.Blend({0.5f, 0.5f, 0.5f, 1}, true);
    CheckArithmetic(additive, 1.5f);
    CheckInterpretation(additive.red);
    FO4HDR_CHECK(std::pow(static_cast<double>(halfOpacity.red), 2.2) < halfOpacity.red);
    FO4HDR_CHECK(std::pow(static_cast<double>(additive.red), 2.2) > additive.red);
}

}  // namespace fo4hdr::tests::gpu
