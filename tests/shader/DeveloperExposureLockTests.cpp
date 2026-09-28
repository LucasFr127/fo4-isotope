// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShippingPixelShaderTests.hpp"
#include "d3d11/GpuHarness.hpp"
#include <d3d11_1.h>
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "d3d11/ScopedD3D11PixelBindings.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>

namespace fo4hdr::tests::gpu {

constexpr Float4 kExposureSceneColor{0.25F,0.5F,1,1};
constexpr Float4 kExposureBloomColor{0.125F,0.25F,0.5F,1};

struct DeveloperExposureBindings
{
    ID3D11Buffer* constants;
    ID3D11ShaderResourceView* slot17;
    ID3D11ShaderResourceView* slot18;
};

void CheckDeveloperExposureBindings(ID3D11DeviceContext& context,
    const DeveloperExposureBindings& expected,
    ID3D11ShaderResourceView* foreign, ID3D11SamplerState* sampler)
{
    ComPtr<ID3D11Buffer> actualCb;
    ComPtr<ID3D11ShaderResourceView> actual7, actual17, actual18;
    ComPtr<ID3D11SamplerState> actualSampler;
    context.PSGetConstantBuffers(13,1,&actualCb);
    context.PSGetShaderResources(7,1,&actual7);
    context.PSGetShaderResources(17,1,&actual17);
    context.PSGetShaderResources(18,1,&actual18);
    context.PSGetSamplers(7,1,&actualSampler);
    FO4HDR_WARP_CHECK(actualCb.Get() == expected.constants);
    FO4HDR_WARP_CHECK(actual17.Get() == expected.slot17);
    FO4HDR_WARP_CHECK(actual18.Get() == expected.slot18);
    FO4HDR_WARP_CHECK(actual7.Get() == foreign);
    FO4HDR_WARP_CHECK(actualSampler.Get() == sampler);
}

void BindDeveloperExposureState(ID3D11DeviceContext& context,
    ID3D11Buffer* cb, ID3D11ShaderResourceView* srv, ID3D11SamplerState* sampler)
{
    context.PSSetConstantBuffers(13,1,&cb);
    context.PSSetShaderResources(7,1,&srv);
    context.PSSetSamplers(7,1,&sampler);
    context.PSSetShaderResources(17,1,&srv);
    context.PSSetShaderResources(18,1,&srv);
}

void VerifyDeveloperExposureBindings(Device& device, ID3D11SamplerState* sampler)
{
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1),
        "Query ID3D11DeviceContext1 for developer exposure bindings");
    const auto foreignConstants = CreateConstantBuffer(*device.device.Get(),
        std::array<float,4>{1,2,3,4});
    const auto ownedConstants = CreateConstantBuffer(*device.device.Get(),
        std::array<float,4>{5,6,7,8});
    const auto foreign = CreateIdentityLut(*device.device.Get());
    const auto owned = CreateSizedTexture(*device.device.Get(), {1,1,
        DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE});
    BindDeveloperExposureState(*device.context.Get(),
        foreignConstants.Get(), foreign.srv.Get(), sampler);

    const auto check = [&](ID3D11Buffer* expectedCb,
                           ID3D11ShaderResourceView* expected17,
                           ID3D11ShaderResourceView* expected18) {
        CheckDeveloperExposureBindings(*device.context.Get(),
            {expectedCb, expected17, expected18}, foreign.srv.Get(), sampler);
    };
    {
        fo4hdr::ScopedD3D11PixelBindings borrowed;
        FO4HDR_WARP_CHECK(borrowed.Bind(
            device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
            7,nullptr,7,nullptr,false,17,owned.srv.Get(),false}));
        FO4HDR_WARP_CHECK(borrowed.Active());
        check(ownedConstants.Get(),owned.srv.Get(),foreign.srv.Get());
        FO4HDR_WARP_CHECK(!borrowed.Bind(
            device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
            7,nullptr,7,nullptr,false,17,owned.srv.Get(),false}));
        check(ownedConstants.Get(),owned.srv.Get(),foreign.srv.Get());
        borrowed.Reset();
        borrowed.Reset();
        FO4HDR_WARP_CHECK(!borrowed.Active());
    }
    check(foreignConstants.Get(),foreign.srv.Get(),foreign.srv.Get());
    {
        fo4hdr::ScopedD3D11PixelBindings extraOnly;
        // An inactive primary slot is not a collision with an extra SRV.
        FO4HDR_WARP_CHECK(extraOnly.Bind(
            device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
            17,nullptr,7,nullptr,false,17,owned.srv.Get(),false}));
        check(ownedConstants.Get(),owned.srv.Get(),foreign.srv.Get());
    }
    check(foreignConstants.Get(),foreign.srv.Get(),foreign.srv.Get());
    fo4hdr::ScopedD3D11PixelBindings failed;
    const auto unchanged = [&] {
        FO4HDR_WARP_CHECK(!failed.Active());
        check(foreignConstants.Get(),foreign.srv.Get(),foreign.srv.Get());
    };
    FO4HDR_WARP_CHECK(!failed.Bind(
        device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
        7,nullptr,7,nullptr,false,17,owned.srv.Get(),true}));
    unchanged();
    FO4HDR_WARP_CHECK(!failed.Bind(
        device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
        7,nullptr,7,nullptr,false,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
        owned.srv.Get(),false}));
    unchanged();
    FO4HDR_WARP_CHECK(!failed.Bind(
        device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
        7,owned.srv.Get(),7,sampler,true,7,owned.srv.Get(),false}));
    unchanged();
    // Failed preflight must release captured references and remain reusable.
    FO4HDR_WARP_CHECK(failed.Bind(
        device.context.Get(),context1.Get(), {13,ownedConstants.Get(),
        7,nullptr,7,nullptr,false,17,owned.srv.Get(),false}));
    failed.Reset();
    unchanged();
    BindDeveloperExposureState(*device.context.Get(), nullptr, nullptr, nullptr);
}

Texture CaptureDeveloperExposurePair(Device& device)
{
    const auto historyShader = CreateShader(*device.device.Get(),
        fo4hdr::generated::kDeveloperExposureCaptureCs);
    auto snapshot = CreateSizedTexture(*device.device.Get(), { 1, 1,
        DXGI_FORMAT_R32G32_FLOAT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE});
    const auto capture = [&](float measured, Float4 bounds, float expected,
                             UINT width = 1U) {
        std::array<float, 2> samples{measured,measured};
        D3D11_SUBRESOURCE_DATA data{samples.data(),
            static_cast<UINT>(width * sizeof(float)), 0};
        auto input = CreateSizedTexture(*device.device.Get(), {width,1,
            DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,&data});
        const std::array<Float4,2> values{{{},bounds}};
        const auto constants = CreateConstantBuffer(*device.device.Get(),values);
        auto* srv = input.srv.Get();
        auto* uav = snapshot.uav.Get();
        auto* cb = constants.Get();
        device.context->CSSetShaderResources(0,1,&srv);
        device.context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        device.context->CSSetConstantBuffers(2,1,&cb);
        device.context->CSSetShader(historyShader.Get(),nullptr,0);
        device.context->Dispatch(1,1,1);
        srv = nullptr; uav = nullptr; cb = nullptr;
        device.context->CSSetShaderResources(0,1,&srv);
        device.context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        device.context->CSSetConstantBuffers(2,1,&cb);
        device.context->CSSetShader(nullptr,nullptr,0);
        device.context->CopyResource(snapshot.staging.Get(),snapshot.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(snapshot.staging.Get(),0,D3D11_MAP_READ,0,&mapped),
            "Map developer exposure pair");
        const auto* pair = static_cast<const float*>(mapped.pData);
        FO4HDR_WARP_CHECK(std::isfinite(pair[0]) && std::isfinite(pair[1]));
        FO4HDR_WARP_CHECK(std::abs(pair[0]-expected) < 1.0e-5F);
        FO4HDR_WARP_CHECK(pair[1] == (expected > 0 ? measured : 0));
        device.context->Unmap(snapshot.staging.Get(),0);
    };
    constexpr Float4 bounds{4,0.25F,2.002F,0};
    capture(0,bounds,4);
    capture(100,bounds,0.25F);
    for (const auto invalid : {NAN,INFINITY,-INFINITY,-1.0F})
        capture(invalid,bounds,0);
    capture(1,{0.25F,4,2.002F,0},0);
    capture(1,{4,-1,2.002F,0},0);
    capture(1,{4,0,2.002F,0},0);
    capture(1,{4,0.25F,0,0},0);
    capture(1,{INFINITY,0.25F,2.002F,0},0);
    capture(1,{4,0.25F,NAN,0},0);
    capture(1,bounds,0,2);
    // Keep this real GPU-produced pair bound for the tone-family consumer.
    capture(1,bounds,2);

    return snapshot;
}

struct DeveloperExposureExpected
{
    float effective;
    float filterMeasured;
    float compensation;
    std::size_t variant;
};

void CheckDeveloperExposurePixels(const std::array<Float4,kWidth>& actual,
    const DeveloperExposureExpected& expectedState)
{
    const std::array source{kExposureSceneColor.red,kExposureSceneColor.green,kExposureSceneColor.blue};
    const std::array bloomChannels{kExposureBloomColor.red,kExposureBloomColor.green,kExposureBloomColor.blue};
    const std::array affine{0.2F,0.3F,0.4F};
    for (const auto& pixel : actual) {
        const std::array channels{pixel.red,pixel.green,pixel.blue};
        for (std::size_t c = 0; c < 3; ++c) {
            float expected = ((source[c]+bloomChannels[c]*0.5F)*expectedState.effective)*0.5F + expectedState.filterMeasured*0.5F;
            if ((expectedState.variant&1U) != 0) expected += 0.25F*(affine[c]-expected);
            expected *= expectedState.compensation;
            FO4HDR_WARP_CHECK(std::isfinite(channels[c]));
            FO4HDR_WARP_CHECK(std::abs(channels[c]-expected) <= 3.0e-5F);
        }
        FO4HDR_WARP_CHECK(pixel.alpha == 1);
    }
}

template <class Draw>
void VerifyDeveloperExposureSweep(const Draw& draw, std::size_t variant,
    ID3D11ShaderResourceView* snapshot)
{
    for (const bool locked : {false,true})
    for (const float measured : {0.25F,4.0F})
    for (const float compensation : {0.5F,2.0F}) {
        constexpr Float4 changedBounds{0.75F,0.5F,0.501F,0};
        const auto actual = draw(measured,changedBounds,locked,compensation,false,snapshot);
        const float effective = locked ? 2.0F :
            std::clamp(changedBounds.blue/(0.001F+measured),changedBounds.green,changedBounds.red);
        const float filterMeasured = locked ? 1.0F : measured;
        CheckDeveloperExposurePixels(actual,
            {effective, filterMeasured, compensation, variant});
    }
}

template <class Draw>
void VerifyInvalidDeveloperExposurePairs(Device& device, const Draw& draw,
    Float4 bounds, ID3D11ShaderResourceView* snapshot)
{
    const auto baseline = draw(4,bounds,false,1,false,snapshot);
    // Invalid snapshot pairs must use the live native calculation.
    for (const std::array<float,2> invalid : {
        std::array<float,2>{0,1}, {2,-1}, {NAN,1}, {2,INFINITY}}) {
        D3D11_SUBRESOURCE_DATA data{invalid.data(),sizeof(invalid),0};
        auto bad = CreateSizedTexture(*device.device.Get(), {1,1,DXGI_FORMAT_R32G32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,&data});
        const auto fallback = draw(4,bounds,true,1,false,bad.srv.Get());
        for (std::size_t i = 0; i < kWidth; ++i) {
            FO4HDR_WARP_CHECK(fallback[i].red == baseline[i].red);
            FO4HDR_WARP_CHECK(fallback[i].green == baseline[i].green);
            FO4HDR_WARP_CHECK(fallback[i].blue == baseline[i].blue);
        }
    }
}

template <class Draw>
void VerifyDeveloperExposureMaskBypass(const Draw& draw, Float4 bounds,
    ID3D11ShaderResourceView* snapshot)
{
    // Mask class 4 bypasses both the lock and artistic compensation.
    const auto bypass = draw(4,bounds,true,2,true,snapshot);
    for (const auto& pixel : bypass) {
        FO4HDR_WARP_CHECK(pixel.red == kExposureSceneColor.red);
        FO4HDR_WARP_CHECK(pixel.green == kExposureSceneColor.green);
        FO4HDR_WARP_CHECK(pixel.blue == kExposureSceneColor.blue);
    }
}

void VerifyDeveloperExposureLock(Device& device,
    ID3D11VertexShader* vertexShader, ID3D11SamplerState* sampler)
{
    VerifyDeveloperExposureBindings(device,sampler);
    auto snapshot = CaptureDeveloperExposurePair(device);
    constexpr Float4 bounds{4,0.25F,2.002F,0};

    const auto makeInput = [&](Float4 color) {
        const auto pixels = MakeFp16Pixels(UniformPixels(color));
        D3D11_SUBRESOURCE_DATA data{pixels.data(),sizeof(pixels),0};
        return CreateTexture(*device.device.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,&data);
    };
    auto scene = makeInput(kExposureSceneColor);
    auto bloom = makeInput(kExposureBloomColor);
    auto depth = makeInput({});
    auto masked = makeInput({0,0,0,4.0F/255.0F});
    auto output = CreateTexture(*device.device.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D11_BIND_RENDER_TARGET);
    std::array<ComPtr<ID3D11PixelShader>,4> shaders{
        CreatePixelShader(*device.device.Get(),fo4hdr::generated::kToneMapA),
        CreatePixelShader(*device.device.Get(),fo4hdr::generated::kToneMapB),
        CreatePixelShader(*device.device.Get(),fo4hdr::generated::kToneMapNewA),
        CreatePixelShader(*device.device.Get(),fo4hdr::generated::kToneMapNewB)};
    std::array<ID3D11SamplerState*,4> samplers{sampler,sampler,sampler,sampler};
    for (std::size_t variant = 0; variant < shaders.size(); ++variant) {
        // A non-neutral z makes the scene filter depend on raw measured
        // exposure; freezing only the effective multiplier fails this test.
        const auto draw = [&](float measured, Float4 currentBounds, bool locked,
                              float compensation, bool mask,
                              ID3D11ShaderResourceView* pairSrv) {
            auto exposure = makeInput({measured,0,0,1});
            ImageSpaceConstantsWithFinalAffine image{};
            image.values[1] = currentBounds;
            image.values[2] = {1,0,0.5F,1};
            image.values[4] = {1,1,1,1};
            image.values[5] = {0.2F,0.3F,0.4F,0.25F};
            const auto imageConstants = CreateConstantBuffer(*device.device.Get(),image);
            ToneMapConstants settings{0,1000,203,203,compensation,203,0,0};
            settings.liveSettingsReserved = locked ? 1U : 0U;
            settings.bloomStrength = 0.5F;
            const auto settingsConstants = CreateConstantBuffer(*device.device.Get(),settings);
            std::array<ID3D11ShaderResourceView*,18> resources{};
            resources[0] = variant >= 2 ? scene.srv.Get() : bloom.srv.Get();
            resources[1] = variant >= 2 ? bloom.srv.Get() : scene.srv.Get();
            resources[2] = exposure.srv.Get();
            resources[3] = mask ? masked.srv.Get() : depth.srv.Get();
            resources[17] = pairSrv;
            auto* target = output.rtv.Get();
            DrawFullscreen(*device.context.Get(), {vertexShader,shaders[variant].Get(),
                imageConstants.Get(),settingsConstants.Get(),resources.data(),18,
                samplers.data(),4,&target,1});
            ID3D11ShaderResourceView* nullSrv{};
            device.context->PSSetShaderResources(17,1,&nullSrv);
            device.context->CopyResource(output.staging.Get(),output.texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CheckHresult(device.context->Map(output.staging.Get(),0,D3D11_MAP_READ,0,&mapped),
                "Map locked tone-family output");
            std::array<Float4,kWidth> result{};
            std::copy_n(static_cast<const Float4*>(mapped.pData),kWidth,result.begin());
            device.context->Unmap(output.staging.Get(),0);
            return result;
        };
        VerifyDeveloperExposureSweep(draw, variant, snapshot.srv.Get());
        VerifyDeveloperExposureMaskBypass(draw, bounds, snapshot.srv.Get());
        VerifyInvalidDeveloperExposurePairs(device, draw, bounds, snapshot.srv.Get());
    }
    std::cout << "Native grading developer exposure: pair capture, four tone variants, live bypass, compensation, scene filter, mask and invalid fallback checked\n";
}

}  // namespace fo4hdr::tests::gpu
