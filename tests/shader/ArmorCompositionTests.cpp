// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include <d3d11_1.h>
#include "reference/ShaderReference.hpp"
#include "Fallout4HDR/ShippingShaders.generated.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "passes/ArmorEncodedComposition.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace fo4hdr::tests::gpu {
namespace {

// Stand-in for the captured next setter: record both private transitions and
// forward to D3D without running a renderer target-observation callback.
UINT g_armorEncodedSetterCalls{};
ID3D11RenderTargetView* g_armorEncodedExpectedRtv{};
void STDMETHODCALLTYPE ArmorEncodedNextSetterSpy(ID3D11DeviceContext* context,
    UINT targetCount, ID3D11RenderTargetView* const* targets,
    ID3D11DepthStencilView* depth)
{
    FO4HDR_WARP_CHECK(depth==nullptr);
    if (g_armorEncodedSetterCalls==0) {
        FO4HDR_WARP_CHECK(targetCount==0 && targets==nullptr);
    } else {
        FO4HDR_WARP_CHECK(g_armorEncodedSetterCalls==1);
        FO4HDR_WARP_CHECK(targetCount==D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && targets!=nullptr);
        if (targets && targetCount==D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
            FO4HDR_WARP_CHECK(targets[0]==g_armorEncodedExpectedRtv);
            for (UINT index=1;index<targetCount;++index) FO4HDR_WARP_CHECK(targets[index]==nullptr);
        }
    }
    ++g_armorEncodedSetterCalls;
    context->OMSetRenderTargets(targetCount,targets,depth);
}

template <class Dispatch>
void VerifyOccupiedArmorUav(ID3D11DeviceContext& context,
    ID3D11Predicate* predicate, ID3D11UnorderedAccessView* occupied,
    const Dispatch& dispatch)
{
    // Any occupied CS or OM UAV must reject without unbinding or dispatching.
    for (bool compute : {false,true}) {
        context.ClearState();
        auto* uav=occupied;
        if (compute) context.CSSetUnorderedAccessViews(3,1,&uav,nullptr);
        else context.OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,3,1,&uav,nullptr);
        context.SetPredication(predicate,TRUE);
        g_armorEncodedSetterCalls=0;
        FO4HDR_WARP_CHECK(!dispatch());
        FO4HDR_WARP_CHECK(g_armorEncodedSetterCalls==0);
        ComPtr<ID3D11UnorderedAccessView> actual;
        if (compute) context.CSGetUnorderedAccessViews(3,1,&actual);
        else context.OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,3,1,&actual);
        FO4HDR_WARP_CHECK(actual.Get()==uav);
        ComPtr<ID3D11Predicate> actualPredicate;
        BOOL value{};
        context.GetPredication(&actualPredicate,&value);
        FO4HDR_WARP_CHECK(actualPredicate.Get()==predicate && value==TRUE);
    }
}

struct ArmorEncodedExpectedState
{
    ID3D11RenderTargetView* rtv;
    ID3D11ShaderResourceView* srv;
    ID3D11Buffer* buffer;
    UINT first;
    UINT count;
    ID3D11ComputeShader* shader;
    ID3D11Predicate* predicate;
};

void CheckArmorEncodedState(ID3D11DeviceContext& context,
    ID3D11DeviceContext1& context1, const ArmorEncodedExpectedState& expected)
{
    ComPtr<ID3D11RenderTargetView> actualRtv;
    context.OMGetRenderTargets(1,&actualRtv,nullptr);
    FO4HDR_WARP_CHECK(actualRtv.Get()==expected.rtv);
    const auto checkSrv=[&](auto getter) {
        ComPtr<ID3D11ShaderResourceView> actual;
        (context.*getter)(127,1,&actual);
        FO4HDR_WARP_CHECK(actual.Get()==expected.srv);
    };
    checkSrv(&ID3D11DeviceContext::VSGetShaderResources);
    checkSrv(&ID3D11DeviceContext::HSGetShaderResources);
    checkSrv(&ID3D11DeviceContext::DSGetShaderResources);
    checkSrv(&ID3D11DeviceContext::GSGetShaderResources);
    checkSrv(&ID3D11DeviceContext::PSGetShaderResources);
    checkSrv(&ID3D11DeviceContext::CSGetShaderResources);
    ComPtr<ID3D11Buffer> actualBuffer;
    UINT actualFirst{},actualCount{};
    context1.CSGetConstantBuffers1(13,1,&actualBuffer,&actualFirst,&actualCount);
    FO4HDR_WARP_CHECK(actualBuffer.Get()==expected.buffer && actualFirst==expected.first && actualCount==expected.count);
    ComPtr<ID3D11ComputeShader> actualShader;
    context.CSGetShader(&actualShader,nullptr,nullptr);
    FO4HDR_WARP_CHECK(actualShader.Get()==expected.shader);
    ComPtr<ID3D11Predicate> actualPredicate;
    BOOL actualValue{};
    context.GetPredication(&actualPredicate,&actualValue);
    FO4HDR_WARP_CHECK(actualPredicate.Get()==expected.predicate && actualValue==TRUE);
}

void VerifyArmorEncodedState(Device& device, ID3D11ComputeShader* shader,
    ID3D11Buffer* constants, Texture& target)
{
    auto& gpu=*device.device.Get();
    auto& context=*device.context.Get();
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1),"Query armor fixture context1");
    auto other=CreateSizedTexture(gpu, {13,3,DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS});
    const auto ranged=CreateConstantBuffer(gpu,std::array<float,128>{});
    UINT first=16, count=16;
    auto* rangedBinding=ranged.Get();
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};
    ComPtr<ID3D11Predicate> predicate;
    CheckHresult(gpu.CreatePredicate(&queryDesc,&predicate),"Create armor predicate");
    context.Begin(predicate.Get());
    context.End(predicate.Get());
    for (bool aliasRtv : {false,true}) {
        context.ClearState();
        constexpr float initial[4]{4,4,4,.5F};
        context.ClearRenderTargetView(target.rtv.Get(),initial);
        auto* rtv=aliasRtv ? target.rtv.Get() : other.rtv.Get();
        context.OMSetRenderTargets(1,&rtv,nullptr);
        // The SRV case binds the target at slot 127 in all six stages;
        // the RTV case binds it as the output target with those SRVs unbound.
        auto* srv=aliasRtv ? nullptr : target.srv.Get();
        context.VSSetShaderResources(127,1,&srv);
        context.HSSetShaderResources(127,1,&srv);
        context.DSSetShaderResources(127,1,&srv);
        context.GSSetShaderResources(127,1,&srv);
        context.PSSetShaderResources(127,1,&srv);
        context.CSSetShaderResources(127,1,&srv);
        context.CSSetShader(shader,nullptr,0);
        context1->CSSetConstantBuffers1(13,1,&rangedBinding,&first,&count);
        context.SetPredication(predicate.Get(),TRUE);
        const ArmorEncodedExpectedState expected{rtv, srv, ranged.Get(),
            first, count, shader, predicate.Get()};
        g_armorEncodedSetterCalls=0;
        g_armorEncodedExpectedRtv=rtv;
        FO4HDR_WARP_CHECK(DispatchArmorEncodedComposition(&context,context1.Get(),{shader,target.uav.Get(),constants,13,3},&ArmorEncodedNextSetterSpy));
        FO4HDR_WARP_CHECK(g_armorEncodedSetterCalls==2);
        CheckArmorEncodedState(context, *context1.Get(), expected);
        context.SetPredication(nullptr,FALSE);
        context.CopyResource(target.staging.Get(),target.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(context.Map(target.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map predicated armor output");
        const auto* pixel=static_cast<const std::uint16_t*>(mapped.pData);
        FO4HDR_WARP_CHECK(std::abs(Binary16ToFloat(pixel[0])-1.87786F)<.003F);
        FO4HDR_WARP_CHECK(Binary16ToFloat(pixel[3])==.5F);
        context.Unmap(target.staging.Get(),0);
        context.SetPredication(predicate.Get(),TRUE);
        g_armorEncodedSetterCalls=0;
        FO4HDR_WARP_CHECK(!DispatchArmorEncodedComposition(&context,context1.Get(),{shader,target.uav.Get(),constants,0,3},&ArmorEncodedNextSetterSpy));
        FO4HDR_WARP_CHECK(g_armorEncodedSetterCalls==0);
        CheckArmorEncodedState(context, *context1.Get(), expected);
    }
    VerifyOccupiedArmorUav(context, predicate.Get(), other.uav.Get(), [&] {
        return DispatchArmorEncodedComposition(&context,context1.Get(),{shader,target.uav.Get(),constants,13,3},&ArmorEncodedNextSetterSpy);
    });
    context.ClearState();
    g_armorEncodedExpectedRtv=nullptr;
}

}  // namespace

// Project-owned oracle: double precision via XYZ, independent of GPU matrix
// factorization. Blend equations are evaluated against actual encoded readback.
using ArmorRgb = std::array<double, 3>;

ArmorRgb ArmorOracleGamut(const ArmorRgb& rgb, bool encode)
{
    using Matrix = std::array<ArmorRgb, 3>;
    constexpr Matrix rec2020ToXyz{{{.6369580483,.1446169036,.1688809752},
        {.2627002120,.6779980715,.0593017165},{0,.0280726930,1.0609850577}}};
    constexpr Matrix xyzToRec709{{{3.2409699419,-1.5373831776,-.4986107603},
        {-.9692436363,1.8759675015,.0415550574},{.0556300797,-.2039769589,1.0569715142}}};
    constexpr Matrix rec709ToXyz{{{.4123907993,.3575843394,.1804807884},
        {.2126390059,.7151686788,.0721923154},{.0193308187,.1191947798,.9505321522}}};
    constexpr Matrix xyzToRec2020{{{1.7166511880,-.3556707838,-.2533662814},
        {-.6666843518,1.6164812366,.0157685458},{.0176398574,-.0427706133,.9421031212}}};
    const auto multiply = [](const Matrix& matrix, const ArmorRgb& v) {
        ArmorRgb result{};
        for (std::size_t row=0; row<3; ++row)
            for (std::size_t col=0; col<3; ++col) result[row] += matrix[row][col]*v[col];
        return result;
    };
    return encode ? multiply(xyzToRec709, multiply(rec2020ToXyz, rgb))
                  : multiply(xyzToRec2020, multiply(rec709ToXyz, rgb));
}

ArmorRgb ArmorOracleTransform(ArmorRgb value, bool encode)
{
    if (encode) value = ArmorOracleGamut(value, true);
    for (auto& channel : value)
        channel = std::copysign(std::pow(std::abs(channel), encode ? 1.0/2.2 : 2.2), channel);
    if (!encode) value = ArmorOracleGamut(value, false);
    return value;
}

struct ArmorEncodedFixture
{
    static constexpr UINT width=13, height=3; // Partial groups on both axes.
    using Pixel = std::array<float,4>;
    using Pixels = std::array<Pixel,width*height>;
    struct PeakConstants { UINT encode; float peakUiRelative; UINT reserved[2]; };
    Device& device;
    ID3D11Device& gpu;
    ID3D11DeviceContext& context;
    Texture target;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> encodeBuffer, decodeBuffer, productionDecodeBuffer;
    ComPtr<ID3D11DeviceContext1> context1;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RasterizerState> raster;
    static constexpr Pixel needle{4.594793797F,.181727678F,.011217758F,1};
    int worstNeedleBlendUlps{};
    double worstEncode{}, worstRoundtrip{};

    explicit ArmorEncodedFixture(Device& inputDevice) :
        device(inputDevice), gpu(*device.device.Get()), context(*device.context.Get())
    {
        target = CreateSizedTexture(gpu, {width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE});
        shader = CreateShader(gpu,fo4hdr::generated::kArmorEncodedCompositionComputeShader);
        encodeBuffer = CreateConstantBuffer(gpu,std::array<UINT,4>{1,0,0,0});
        decodeBuffer = CreateConstantBuffer(gpu,std::array<UINT,4>{0,0,0,0});
        productionDecodeBuffer=CreateConstantBuffer(gpu,PeakConstants{0,430.0F/203.0F,{0,0}});
        CheckHresult(device.context.As(&context1),"Query armor encoded context1");
        }

    static float quantize(double value) { return Binary16ToFloat(FloatToBinary16(static_cast<float>(value))); }
    void upload(const Pixels& values) {
        context.ClearState();
        std::array<std::array<std::uint16_t,4>,width*height> packed{};
        for (std::size_t i=0;i<values.size();++i)
            for (std::size_t c=0;c<4;++c) packed[i][c]=FloatToBinary16(values[i][c]);
        context.UpdateSubresource(target.texture.Get(),0,nullptr,packed.data(),width*8,0);
    }
    Pixels read() {
        context.CopyResource(target.staging.Get(),target.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(context.Map(target.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map armor encoded fixture");
        Pixels values{};
        for (UINT y=0;y<height;++y) {
            const auto* row = reinterpret_cast<const std::uint16_t*>(
                static_cast<const std::byte*>(mapped.pData)+y*mapped.RowPitch);
            for (UINT x=0;x<width;++x) for (UINT c=0;c<4;++c)
                values[y*width+x][c]=Binary16ToFloat(row[x*4+c]);
        }
        context.Unmap(target.staging.Get(),0);
        return values;
    }
    void dispatch(bool encode) {
        auto* constants=encode ? encodeBuffer.Get() : decodeBuffer.Get();
        FO4HDR_WARP_CHECK(DispatchArmorEncodedComposition(&context,context1.Get(),
            {shader.Get(),target.uav.Get(),constants,width,height}));
    }
    void VerifyExtremes()
    {
        // Emergency finite-storage bound is separate from ordinary HDR behavior.
        Pixels extreme{};
        extreme.fill({65504,-65504,65504,.625F});
        upload(extreme);
        dispatch(false);
        CheckFiniteExtremes(read());
        extreme.fill({NAN,INFINITY,-INFINITY,.25F});
        for (bool encode : {false,true}) {
            upload(extreme);
            dispatch(encode);
            for (const auto& pixel:read()) {
                for (std::size_t c=0;c<3;++c) FO4HDR_WARP_CHECK(std::isfinite(pixel[c]));
                FO4HDR_WARP_CHECK(pixel[3]==.25F);
            }
        }
    }

    void VerifyRoundtrip()
    {
        constexpr std::array<Pixel,13> seeds{{{0,0,0,0},{1,1,1,1},{4,4,4,.5F},
            {1,0,0,.25F},{0,1,0,2},{0,0,1,-1},{-.25F,.5F,3,.75F},
            {16,2,.01F,.125F},{.001F,.0001F,.00001F,-0.0F},
            {-1,-2,-3,1},{64,.25F,16,.375F},{.18F,.18F,.18F,.625F},{.25F,4,.5F,.875F}}};
        constexpr std::array<double, height> rowScales{1.0, .03125, 4.0};
        Pixels input{};
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
            input[y*width+x]=seeds[x];
            for (UINT c=0;c<3;++c) input[y*width+x][c]=quantize(seeds[x][c]*rowScales[y]);
        }
        upload(input);
        dispatch(true);
        const auto encoded=read();
        CheckEncoded(input, encoded);
        FO4HDR_WARP_CHECK(encoded[3][1]<0 && encoded[3][2]<0); // Wide gamut survives as signed code.
        FO4HDR_WARP_CHECK(encoded[2][0]>1 && encoded[0][0]==0);
        dispatch(false);
        const auto roundtrip=read();
        CheckRoundtrip(input, roundtrip);
    }

    void PrepareDraw()
    {
        vs=CreateVertexShader(gpu,fo4hdr::generated::test::kFullscreenVs);
        ps=CreatePixelShader(gpu,fo4hdr::generated::test::kArmorEncodedSourcePs);
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode=D3D11_FILL_SOLID;
        rasterDesc.CullMode=D3D11_CULL_NONE;
        CheckHresult(gpu.CreateRasterizerState(&rasterDesc,&raster),"Create armor encoded raster state");
    }

    struct BlendCase
    {
        UINT mode;
        std::size_t sourceIndex;
        Pixel source;
    };
    struct BlendResult
    {
        const Pixels& before;
        const Pixels& blended;
        const Pixels& decoded;
    };
    static ArmorRgb ExpectedBlendCode(const Pixel& source, const Pixel& before, UINT mode)
    {
        ArmorRgb expectedCode{};
        for (std::size_t c=0;c<3;++c)
            expectedCode[c]=quantize(source[c]*(mode==1 ? 1 : source[3])+
                before[c]*(mode==0 ? 1-source[3] : 1));
        return expectedCode;
    }
    void CheckBlend(const BlendCase& test, const BlendResult& result)
    {
        for (std::size_t i=0;i<result.decoded.size();++i) {
            const auto expectedCode = ExpectedBlendCode(test.source, result.before[i], test.mode);
            for (std::size_t c=0;c<3;++c)
                FO4HDR_WARP_CHECK(std::abs(result.blended[i][c]-expectedCode[c])<.003);
            const auto expected=ArmorOracleTransform(expectedCode,false);
            for (std::size_t c=0;c<3;++c)
                FO4HDR_WARP_CHECK(std::abs(result.decoded[i][c]-expected[c])<.005*std::max(1.0,std::abs(expected[c])));
            FO4HDR_WARP_CHECK(result.decoded[i][3]==.375F);
        }
        if (test.mode==0 && test.sourceIndex==0) FO4HDR_WARP_CHECK(std::abs(result.decoded[0][0]-.21763764F)<.001F);
        if (test.sourceIndex==1) FO4HDR_WARP_CHECK(result.blended[0][0]>1 && result.decoded[0][0]>1);
    }

    void VerifyBlends()
    {
        // Native SRC_ALPHA/INV_SRC_ALPHA, ONE/ONE, and SRC_ALPHA/ONE RGB blends.
        // Source > 1 is deliberately present, and the 50% black case distinguishes
        // encoded-domain composition (0.5^2.2) from linear attenuation (0.5).
        constexpr std::array<Pixel,4> sources{{{0,0,0,.5F},{2,1.5F,.25F,.5F},
            {-.25F,.25F,3,.25F},{.75F,.125F,.5F,1}}};
        for (UINT mode=0;mode<3;++mode) for (std::size_t s=0;s<sources.size();++s) {
            Pixels white{};
            white.fill({1,1,1,.375F});
            upload(white);
            dispatch(true);
            const auto before=read();
            const auto sourceBuffer=CreateConstantBuffer(gpu,sources[s]);
            D3D11_BLEND_DESC desc{};
            auto& blend=desc.RenderTarget[0];
            blend.BlendEnable=TRUE;
            blend.SrcBlend=mode==1 ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA;
            blend.DestBlend=mode==0 ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_ONE;
            blend.BlendOp=D3D11_BLEND_OP_ADD;
            blend.SrcBlendAlpha=D3D11_BLEND_ONE;
            blend.DestBlendAlpha=D3D11_BLEND_ZERO;
            blend.BlendOpAlpha=D3D11_BLEND_OP_ADD;
            blend.RenderTargetWriteMask=7; // Native RGB-only leaves scene alpha alone.
            ComPtr<ID3D11BlendState> blendState;
            CheckHresult(gpu.CreateBlendState(&desc,&blendState),"Create native encoded blend");
            DrawEncoded(sourceBuffer.Get(), blendState.Get());
            const auto blended=read();
            dispatch(false);
            const auto decoded=read();
            CheckBlend({mode, s, sources[s]}, {before, blended, decoded});
        }
    }

    struct NeedleCase
    {
        float peak, white, relativePeak, background, opacity;
    };
    void CheckNeedle(const Pixels& before, const Pixels& native, const Pixels& limited,
        const NeedleCase& test)
    {
        for (std::size_t i=0;i<limited.size();++i) {
            CheckNeedleBlend(before[i], native[i], test.opacity);
            CheckNeedleLimiter(native[i], limited[i], test.relativePeak);
        }
        CheckNeedleAnchor(limited, test);
    }

    void CheckNeedleBlend(const Pixel& before, const Pixel& native, float opacity)
    {
        ArmorRgb expectedCode{};
        for (std::size_t c=0;c<3;++c) {
            expectedCode[c]=quantize(needle[c]*opacity+before[c]);
            // These nonnegative fixtures permit monotonic binary16 bit
            // distances. WARP's blend/source precision can differ by one
            // representable step from FP32 blending then nearest-half
            // conversion; at code 4..8 that step is .00390625.
            const auto ulps=std::abs(int(FloatToBinary16(native[c]))-
                int(FloatToBinary16(static_cast<float>(expectedCode[c]))));
            worstNeedleBlendUlps=std::max(worstNeedleBlendUlps,ulps);
            FO4HDR_WARP_CHECK(std::isfinite(native[c]) && native[c]>=0 && ulps<=1);
        }
    }

    static void CheckNeedleLimiter(const Pixel& native, const Pixel& limited, float relativePeak)
    {
        // Independently test the limiter from its actual encoded input;
        // the separate ULP assertion above tests native blend arithmetic.
        const auto raw=ArmorOracleTransform({native[0],native[1],native[2]},false);
        const double scale=std::min(1.0,double(relativePeak)/std::max({raw[0],raw[1],raw[2]}));
        for (std::size_t c=0;c<3;++c) {
            FO4HDR_WARP_CHECK(std::abs(limited[c]-raw[c]*scale)<.004*std::max(1.0,raw[c]*scale));
            FO4HDR_WARP_CHECK(limited[c]<=relativePeak+.002F*relativePeak);
        }
        // Check the hue-bearing ratios separately from peak magnitude.
        FO4HDR_WARP_CHECK(std::abs(limited[1]/limited[0]-raw[1]/raw[0])<.002);
        FO4HDR_WARP_CHECK(std::abs(limited[2]/limited[0]-raw[2]/raw[0])<.002);
        FO4HDR_WARP_CHECK(limited[3]==.375F);
    }

    static void CheckNeedleAnchor(const Pixels& limited, const NeedleCase& test)
    {
        if (test.peak!=430 || test.white!=203) return;
        if (test.opacity!=1 || test.background!=0) return;
        FO4HDR_WARP_CHECK(std::abs(limited[0][0]*test.white-430)<.5F);
        FO4HDR_WARP_CHECK(std::abs(limited[0][1]*test.white-47.85F)<.5F);
        FO4HDR_WARP_CHECK(std::abs(limited[0][2]*test.white-11.28F)<.5F);
    }

    void VerifyBelowPeak(ID3D11Buffer* peakBuffer)
    {
        // Ordinary below-peak code decodes identically, including alpha.
        Pixels inRange{};
        inRange.fill({.5F,.25F,.125F,.625F});
        upload(inRange);
        dispatch(false);
        const auto raw=read();
        upload(inRange);
        FO4HDR_WARP_CHECK(DispatchArmorEncodedComposition(&context,context1.Get(),{shader.Get(),target.uav.Get(),peakBuffer,width,height}));
        const auto limited=read();
        for (std::size_t i=0;i<raw.size();++i) for (std::size_t c=0;c<4;++c)
            FO4HDR_WARP_CHECK(raw[i][c]==limited[i][c]);
    }

    void VerifyNeedle()
    {
        // Numeric captured-source replay: supra-one needle code with the observed
        // SRC_ALPHA + ONE blend. This checks proportional BT.2020 limiting and
        // chromatic ratios, without reproducing the live material shader.

        D3D11_BLEND_DESC needleBlendDesc{};
        auto& needleBlend=needleBlendDesc.RenderTarget[0];
        needleBlend.BlendEnable=TRUE;
        needleBlend.SrcBlend=D3D11_BLEND_SRC_ALPHA;
        needleBlend.DestBlend=D3D11_BLEND_ONE;
        needleBlend.BlendOp=D3D11_BLEND_OP_ADD;
        needleBlend.SrcBlendAlpha=D3D11_BLEND_ONE;
        needleBlend.DestBlendAlpha=D3D11_BLEND_ZERO;
        needleBlend.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        needleBlend.RenderTargetWriteMask=7;
        ComPtr<ID3D11BlendState> needleBlendState;
        CheckHresult(gpu.CreateBlendState(&needleBlendDesc,&needleBlendState),"Create needle blend fixture");

        // Non-monotonic peak changes catch retained/stale decode constants.
        for (float peak : {400.0F,2000.0F,430.0F,1000.0F}) for (float white : {100.0F,203.0F,300.0F}) {
            const float relativePeak=peak/white;
            const auto peakBuffer=CreateConstantBuffer(gpu,PeakConstants{0,relativePeak,{0,0}});
            for (float background : {0.0F,.02F,4.0F}) for (float opacity : {1.0F,.5F}) {
                Pixels backgrounds{};
                backgrounds.fill({background,background,background,.375F});
                upload(backgrounds);
                dispatch(true);
                const auto before=read();
                auto source=needle;
                source[3]=opacity;
                const auto sourceBuffer=CreateConstantBuffer(gpu,source);
                DrawEncoded(sourceBuffer.Get(), needleBlendState.Get());
                const auto native=read();
                FO4HDR_WARP_CHECK(native[0][0]>1); // The source is never clamped before decode.
                FO4HDR_WARP_CHECK(DispatchArmorEncodedComposition(&context,context1.Get(),{shader.Get(),target.uav.Get(),peakBuffer.Get(),width,height}));
                const auto limited=read();
                CheckNeedle(before, native, limited, {peak, white, relativePeak, background, opacity});
            }
            VerifyBelowPeak(peakBuffer.Get());
        }
        std::cout << "Needle numeric replay: worst native blend error=" << worstNeedleBlendUlps
                  << " binary16 ULP(s).\n";
    }

    void CheckEncoded(const Pixels& input, const Pixels& encoded)
    {
        for (std::size_t i=0;i<input.size();++i) {
            const auto expected=ArmorOracleTransform({input[i][0],input[i][1],input[i][2]},true);
            for (std::size_t c=0;c<3;++c) {
                const auto error=std::abs(encoded[i][c]-expected[c])/std::max(1.0,std::abs(expected[c]));
                worstEncode=std::max(worstEncode,error);
                FO4HDR_WARP_CHECK(std::isfinite(encoded[i][c]) && error<.0015);
            }
            FO4HDR_WARP_CHECK(std::bit_cast<std::uint32_t>(encoded[i][3]) == std::bit_cast<std::uint32_t>(input[i][3]));
        }
    }

    void CheckRoundtrip(const Pixels& input, const Pixels& roundtrip)
    {
        for (std::size_t i=0;i<input.size();++i) {
            const double scale=std::max({1.0,std::abs(double(input[i][0])),std::abs(double(input[i][1])),std::abs(double(input[i][2]))});
            for (std::size_t c=0;c<3;++c) {
                const auto error=std::abs(roundtrip[i][c]-input[i][c])/scale;
                worstRoundtrip=std::max(worstRoundtrip,error);
                FO4HDR_WARP_CHECK(std::isfinite(roundtrip[i][c]) && error<.004);
            }
            FO4HDR_WARP_CHECK(std::bit_cast<std::uint32_t>(roundtrip[i][3]) == std::bit_cast<std::uint32_t>(input[i][3]));
        }

    }

    static void CheckFiniteExtremes(const Pixels& values)
    {
        for (const auto& pixel:values) {
            for (std::size_t c=0;c<3;++c) FO4HDR_WARP_CHECK(std::isfinite(pixel[c]) && std::abs(pixel[c])<=65504);
            FO4HDR_WARP_CHECK(pixel[0]==65504 && pixel[1]==-65504 && pixel[2]==65504 && pixel[3]==.625F);
        }
    }

    void DrawEncoded(ID3D11Buffer* sourceBuffer, ID3D11BlendState* blendState)
    {
        context.ClearState();
        auto* rtv=target.rtv.Get();
        auto* sourceBinding=sourceBuffer;
        context.OMSetRenderTargets(1,&rtv,nullptr);
        context.OMSetBlendState(blendState,nullptr,0xffffffffu);
        context.RSSetState(raster.Get());
        const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        context.RSSetViewports(1,&viewport);
        context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.VSSetShader(vs.Get(),nullptr,0);
        context.PSSetShader(ps.Get(),nullptr,0);
        context.PSSetConstantBuffers(0,1,&sourceBinding);
        context.Draw(3,0);
        context.OMSetRenderTargets(0,nullptr,nullptr);
    }

    void Run()
    {
        VerifyRoundtrip();
        PrepareDraw();
        VerifyBlends();
        VerifyNeedle();
        VerifyExtremes();
        context.ClearState();
        VerifyArmorEncodedState(device,shader.Get(),encodeBuffer.Get(),target);
        std::cout << "Armor encoded FP16: worst relative encode error=" << worstEncode
                  << ", worst scale-normalized roundtrip error=" << worstRoundtrip << '\n';
        if (benchmark) Benchmark();
    }
};

bool VerifyArmorEncodedComposition(Device& device)
{
    auto& gpu = *device.device.Get();
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{DXGI_FORMAT_R16G16B16A16_FLOAT, 0};
    if (FAILED(gpu.CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) ||
        (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)
    {
        std::cout << "SKIP armor encoded composition: R16G16B16A16_FLOAT typed UAV loads unsupported.\n";
        return false;
    }
    ArmorEncodedFixture fixture(device);
    fixture.Run();
    return true;
}

}  // namespace fo4hdr::tests::gpu
