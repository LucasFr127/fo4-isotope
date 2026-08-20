// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "passes/ArmorEncodedComposition.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cmath>
#include <iostream>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;
constexpr UINT armorWidth = 13;
constexpr UINT armorHeight = 3;

bool TypedArmorSupported(ID3D11Device* device)
{
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{DXGI_FORMAT_R16G16B16A16_FLOAT, 0};
    constexpr UINT required = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
        D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,
            &support, sizeof(support)))) return false;
    return (support.OutFormatSupport2 & required) == required;
}

struct ArmorFixture
{
    Device gpu{CreateWarpDevice()};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11UnorderedAccessView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> source;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ArmorEncodedComposition composition;

    void PrepareTarget()
    {
        auto description = SceneDescription(armorWidth, armorHeight);
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        CheckHresult(gpu.device->CreateTexture2D(&description, nullptr, &texture), "Armor target");
        CheckHresult(gpu.device->CreateRenderTargetView(texture.Get(), nullptr, &renderTarget),
            "Armor RTV");
        CheckHresult(gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &target),
            "Armor UAV");
        const float white[4]{1, 1, 1, 0.375f};
        gpu.context->ClearRenderTargetView(renderTarget.Get(), white);
        auto* binding = renderTarget.Get();
        gpu.context->OMSetRenderTargets(1, &binding, nullptr);
    }

    void PrepareDraw(const std::filesystem::path& directory)
    {
        const auto vertexCode = ReadShader(directory / "Fullscreen.dxbc");
        const auto pixelCode = ReadShader(directory / "ArmorEncodedSource.dxbc");
        CheckHresult(gpu.device->CreateVertexShader(vertexCode.data(), vertexCode.size(),
            nullptr, &vertex), "Armor vertex shader");
        CheckHresult(gpu.device->CreatePixelShader(pixelCode.data(), pixelCode.size(),
            nullptr, &pixel), "Armor source shader");
        const std::array<Float4, 1> black{{{0, 0, 0, 0.5f}}};
        source = ConstantBuffer(gpu.device.Get(), black);
        D3D11_RASTERIZER_DESC description{};
        description.FillMode = D3D11_FILL_SOLID;
        description.CullMode = D3D11_CULL_NONE;
        CheckHresult(gpu.device->CreateRasterizerState(&description, &rasterizer),
            "Armor rasterizer");
        PrepareBlend();
    }

    void PrepareBlend()
    {
        D3D11_BLEND_DESC description{};
        auto& value = description.RenderTarget[0];
        value.BlendEnable = TRUE;
        value.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        value.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        value.BlendOp = D3D11_BLEND_OP_ADD;
        value.SrcBlendAlpha = D3D11_BLEND_ONE;
        value.DestBlendAlpha = D3D11_BLEND_ZERO;
        value.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        value.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
            D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        CheckHresult(gpu.device->CreateBlendState(&description, &blend), "Armor RGB blend");
    }

    void CheckTarget()
    {
        ComPtr<ID3D11RenderTargetView> bound;
        gpu.context->OMGetRenderTargets(1, &bound, nullptr);
        FO4HDR_CHECK(bound.Get() == renderTarget.Get());
    }

    void Draw()
    {
        auto* constants = source.Get();
        const D3D11_VIEWPORT viewport{0, 0, float(armorWidth), float(armorHeight), 0, 1};
        gpu.context->RSSetState(rasterizer.Get());
        gpu.context->RSSetViewports(1, &viewport);
        gpu.context->OMSetBlendState(blend.Get(), nullptr, 0xffffffffu);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->VSSetShader(vertex.Get(), nullptr, 0);
        gpu.context->PSSetShader(pixel.Get(), nullptr, 0);
        gpu.context->PSSetConstantBuffers(0, 1, &constants);
        gpu.context->Draw(3, 0);
    }

    void VerifyColoredBlend(ArmorEncodedToken token)
    {
        constexpr std::array<float, 4> input{-0.25f, 0.5f, 1.25f, 0.375f};
        gpu.context->ClearRenderTargetView(renderTarget.Get(), input.data());
        token.serial = 2;
        FO4HDR_CHECK(composition.Begin(token));
        CheckTarget();
        Draw();
        FO4HDR_CHECK(composition.End(token));
        CheckTarget();
        // Signed gamma raises the common code factor to 2.2; the linear
        // primary conversions preserve that RGB scale, below the completed peak.
        const auto scale = std::pow(0.5, 2.2);
        const Float4 expected{float(input[0] * scale), float(input[1] * scale),
            float(input[2] * scale), input[3]};
        for (const auto& value : ReadScene(gpu, texture.Get())) CheckPixel(value, expected);
    }

    void VerifyLifecycle(ArmorEncodedToken token)
    {
        token.serial = 3;
        auto stale = token;
        ++stale.resourceGeneration;
        FO4HDR_CHECK(!composition.Begin(stale));
        FO4HDR_CHECK(composition.Domain() == ArmorCompositionDomain::linearBt2020UiRelative);
        FO4HDR_CHECK(composition.Begin(token));
        FO4HDR_CHECK(!composition.End(stale));
        FO4HDR_CHECK(composition.Domain() == ArmorCompositionDomain::unknown);
        FO4HDR_CHECK(composition.Retire());
        FO4HDR_CHECK(!composition.Begin(token));
        FO4HDR_CHECK(!composition.End(token));
        FO4HDR_CHECK(composition.Domain() == ArmorCompositionDomain::unknown);
    }

    void Verify()
    {
        const ArmorEncodedConfiguration configuration{renderTarget.Get(), target.Get(),
            1, 430.0f / 203.0f, ArmorCompositionDomain::linearBt2020UiRelative};
        FO4HDR_CHECK(composition.Initialize(gpu.device.Get(), gpu.context.Get(), configuration));
        const ArmorEncodedToken token{ArmorCompositionOwner::powerArmor,
            std::this_thread::get_id(), 1, 1, 1, ArmorCompositionScope::qualifiedExclusive, true};
        FO4HDR_CHECK(composition.Begin(token));
        FO4HDR_CHECK(composition.Domain() == ArmorCompositionDomain::encodedBt709);
        CheckTarget();
        Draw();
        FO4HDR_CHECK(composition.End(token));
        FO4HDR_CHECK(composition.Domain() == ArmorCompositionDomain::linearBt2020UiRelative);
        CheckTarget();
        // A 50% black code blend decodes to 0.5^2.2, distinguishing linear attenuation.
        const double expected = std::pow(0.5, 2.2);
        for (const auto& value : ReadScene(gpu, texture.Get()))
        {
            CheckPixel(value, {float(expected), float(expected), float(expected), 0.375f});
            FO4HDR_CHECK(value.alpha == 0.375f);
        }
        VerifyColoredBlend(token);
        VerifyLifecycle(token);
    }
};

}  // namespace

void VerifyArmorEncodedComposition(const std::filesystem::path& directory)
{
    ArmorFixture fixture;
    if (!TypedArmorSupported(fixture.gpu.device.Get()))
    {
        std::cout << "SKIP armor composition: FP16 typed LOAD and STORE unavailable.\n";
        return;
    }
    fixture.PrepareTarget();
    fixture.PrepareDraw(directory);
    fixture.Verify();
}

}  // namespace fo4hdr::tests::gpu
