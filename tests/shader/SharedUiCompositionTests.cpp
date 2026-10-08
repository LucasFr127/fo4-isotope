// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationFacadeFixture.hpp"
#include "resources/SharedUiComposition.hpp"
#include <d3d11_1.h>
#include <array>
#include <cmath>
#include <limits>

namespace fo4hdr::tests::gpu {
namespace {

using HalfPixel = std::array<std::uint16_t, 4>;

struct SharedUiSeedImages
{
    Texture& scene;
    Texture& canvas;
};

Texture FacadeTexture(Device& device, IDXGISwapChain& facade)
{
    Texture result;
    result.texture = FacadeBuffer(facade);
    CheckHresult(device.device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv),
        "Create shared UI facade RTV");
    CheckHresult(device.device->CreateShaderResourceView(
        result.texture.Get(), nullptr, &result.srv),
        "Create shared UI facade SRV");
    D3D11_TEXTURE2D_DESC description{};
    result.texture->GetDesc(&description);
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr, &result.staging),
        "Create shared UI facade readback");
    return result;
}

Texture DescriptorTexture(Device& device, const Texture& source, bool halfSize)
{
    D3D11_TEXTURE2D_DESC description{};
    source.texture->GetDesc(&description);
    return CreateSizedTexture(*device.device.Get(),
        {halfSize ? description.Width / 2 : description.Width,
            halfSize ? description.Height / 2 : description.Height,
            description.Format, description.BindFlags});
}

HalfPixel ReadSeedPixel(Device& device, Texture& image)
{
    device.context->OMSetRenderTargets(0, nullptr, nullptr);
    device.context->CopyResource(image.staging.Get(), image.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(image.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read shared UI seed");
    const auto pixel = *static_cast<const HalfPixel*>(mapped.pData);
    device.context->Unmap(image.staging.Get(), 0);
    return pixel;
}

void VerifySeedWhites(Device& device, ID3D11DeviceContext1* context1,
    SharedUiComposition& seed, const SharedUiSeedImages& images)
{
    const auto& [scene, canvas] = images;
    constexpr float neutral[]{1, 1, 1, 1};
    device.context->ClearRenderTargetView(scene.rtv.Get(), neutral);
    for (const float white : {100.0F, 203.0F}) {
        FO4HDR_WARP_CHECK(seed.Seed(device.context.Get(), context1,
            {scene.srv.Get(), canvas.rtv.Get(), {203, white}}));
        const auto result = ReadSeedPixel(device, canvas);
        const auto linear = 203.0F / white;
        const auto expected = 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
        for (UINT channel = 0; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(std::abs(Binary16ToFloat(result[channel]) - expected) < 0.002F);
        FO4HDR_WARP_CHECK(result[3] == FloatToBinary16(1));
    }
    constexpr float extended[]{2, -0.25F, 0.5F, 1};
    device.context->ClearRenderTargetView(canvas.rtv.Get(), extended);
    FO4HDR_WARP_CHECK(seed.Seed(device.context.Get(), context1,
        {canvas.srv.Get(), canvas.rtv.Get(), {203, 203}}));
    const auto result = ReadSeedPixel(device, canvas);
    FO4HDR_WARP_CHECK(Binary16ToFloat(result[0]) > 1);
    FO4HDR_WARP_CHECK(Binary16ToFloat(result[1]) < 0);
}

void VerifyRejectedSeed(Device& device, ID3D11DeviceContext1* context1,
    SharedUiComposition& seed, const SharedUiSeedImages& images)
{
    const auto& [scene, canvas] = images;
    const auto before = ReadSeedPixel(device, canvas);
    for (const float white : {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN()})
        FO4HDR_WARP_CHECK(!seed.Seed(device.context.Get(), context1,
            {scene.srv.Get(), canvas.rtv.Get(), {203, white}}));
    FO4HDR_WARP_CHECK(!seed.Seed(nullptr, context1,
        {scene.srv.Get(), canvas.rtv.Get(), {203, 203}}));
    FO4HDR_WARP_CHECK(ReadSeedPixel(device, canvas) == before);
    auto small = CreateSizedTexture(*device.device.Get(), {4, 4,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET});
    FO4HDR_WARP_CHECK(!seed.InputsCompatible({small.srv.Get(), canvas.rtv.Get(), {203, 203}}));
    FO4HDR_WARP_CHECK(ReadSeedPixel(device, canvas) == before);
}

} // namespace

void VerifySharedUiComposition(Device& device)
{
    PresentationWindow window;
    const auto native = CreatePresentationChain(device, window.handle, 16, 8);
    const auto facade = CreateFacade(*native.Get());
    auto canvas = FacadeTexture(device, *facade.Get());
    auto scene = DescriptorTexture(device, canvas, false);
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1), "Query shared UI seed context1");
    SharedUiComposition seed;
    FO4HDR_WARP_CHECK(seed.Create(device.device.Get(), 16, 8));
    VerifySeedWhites(device, context1.Get(), seed, {scene, canvas});
    VerifyRejectedSeed(device, context1.Get(), seed, {scene, canvas});
    seed.Reset();
    FO4HDR_WARP_CHECK(!seed.InputsCompatible({scene.srv.Get(), canvas.rtv.Get(), {203, 203}}));
    CheckDebugMessages(device);
}

} // namespace fo4hdr::tests::gpu
