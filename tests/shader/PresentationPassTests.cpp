// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include <dxgi1_4.h>
#include "reference/ShaderReference.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fo4hdr::tests::gpu {

void VerifyPresentationColorSpaces(IDXGISwapChain1& chain)
{
    ComPtr<IDXGISwapChain3> colorChain;
    CheckHresult(chain.QueryInterface(IID_PPV_ARGS(&colorChain)), "Query presentation color-space interface");
    UINT support{};
    constexpr auto sdr = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    constexpr auto hdr = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    CheckHresult(colorChain->CheckColorSpaceSupport(sdr, &support), "Check SDR presentation color space");
    FO4HDR_WARP_CHECK((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0);
    CheckHresult(colorChain->SetColorSpace1(sdr), "Apply SDR presentation color space");
    support = 0;
    CheckHresult(colorChain->CheckColorSpaceSupport(hdr, &support), "Check HDR10 presentation color space");
    if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) {
        CheckHresult(colorChain->SetColorSpace1(hdr), "Apply HDR10 presentation color space");
        CheckHresult(colorChain->SetColorSpace1(sdr), "Restore SDR presentation color space");
        std::cout << "Real chain HDR10-to-SDR API negotiation passed; visible HDR output unverified.\n";
    } else {
        std::cout << "Real chain HDR10 PRESENT support unavailable; SDR API negotiation passed, HDR transition unverified.\n";
    }
}

struct DirectPresentationSample
{
    UINT mode, layers, x, y;
};

bool InsidePresentationWhitePatch(UINT x, UINT y)
{
    const double u = (double(x) + 0.5) / 37, v = (double(y) + 0.5) / 19;
    return u >= 0.2 && u <= 0.8 && v >= 0.2 && v <= 0.8;
}

bool IsDirectPresentationPattern(UINT mode)
{
    return mode >= 10 && mode <= 13;
}

fo4hdr::LinearRgb DirectPresentationPattern(UINT mode, UINT x, UINT y)
{
    constexpr UINT width=37, height=19;
    double level{};
    if (mode == 10) level = std::floor(double(x) / (width - 1) * 31 + 1.0e-6) / 31 * 1125;
    if (mode == 11) {
        level = InsidePresentationWhitePatch(x, y) ? 203 : 0;
    }
    if (mode == 12) {
        constexpr std::array levels{0.0,0.001,0.005,0.01,0.02,0.05,0.1,0.2,0.5,1.0};
        level = levels[x * 10 / width];
    }
    fo4hdr::LinearRgb expected{level,level,level};
    if (mode == 13) {
        constexpr std::array<fo4hdr::LinearRgb,6> patches{{
            {203,0,0},{0,203,0},{0,0,203},{203,203,0},{0,203,203},{203,0,203}}};
        expected = patches[x * 6 / width];
        if (y >= height / 2) expected = fo4hdr::ConvertBt709ToBt2020(expected);
    }
    return expected;
}

fo4hdr::LinearRgb DirectPresentationExpected(const Float4& source,
    const DirectPresentationSample& test)
{
    const auto [mode, layers, x, y] = test;
    fo4hdr::LinearRgb expected{source.red, source.green, source.blue};
    if (layers == 0 && IsDirectPresentationPattern(mode))
        expected = DirectPresentationPattern(mode, x, y);
    if (mode != 0) {
        const double scale = mode == 1 || mode == 2 ? 203.0 : 1.0;
        expected = fo4hdr::EncodeSt2084Pq({std::clamp(expected.red * scale, 0.0, 10000.0),
            std::clamp(expected.green * scale, 0.0, 10000.0), std::clamp(expected.blue * scale, 0.0, 10000.0)});
    }
    return expected;
}

struct DirectPresentationReadback
{
    Texture& reference;
    Texture& output;
    UINT mode, layers;
};

void CheckDirectPresentation(Device& device, const DirectPresentationReadback& test)
{
    const auto& [reference, output, mode, layers] = test;
    constexpr UINT width=37, height=19;
    device.context->CopyResource(reference.staging.Get(), reference.texture.Get());
    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE referenceMap{}, outputMap{};
    CheckHresult(device.context->Map(reference.staging.Get(), 0, D3D11_MAP_READ, 0, &referenceMap), "Map layer oracle");
    CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &outputMap), "Map direct output");
    for (UINT y = 0; y < height; ++y) {
        const auto* source = reinterpret_cast<const Float4*>(static_cast<const std::byte*>(referenceMap.pData) + y * referenceMap.RowPitch);
        const auto* actual = reinterpret_cast<const UINT*>(static_cast<const std::byte*>(outputMap.pData) + y * outputMap.RowPitch);
        for (UINT x = 0; x < width; ++x) {
            const auto expected = DirectPresentationExpected(source[x], {mode, layers, x, y});
            const std::array values{expected.red, expected.green, expected.blue};
            for (UINT channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>((actual[x] >> (10 * channel)) & 1023),
                    fo4hdr::QuantizeR10Unorm(values[channel])) <= 1);
            FO4HDR_WARP_CHECK(actual[x] >> 30 == 3);
        }
    }
    device.context->Unmap(reference.staging.Get(), 0);
    device.context->Unmap(output.staging.Get(), 0);
}

struct DirectPresentationInputs
{
    Texture& scene;
    Texture& layer;
    Texture& output;
    const PresentationShaders* shader;
};

void CheckPresentationOverlayPixel(UINT actual, UINT& hidden, bool remember, bool visible)
{
    if (remember) { hidden = actual; return; }
    if (!visible) { FO4HDR_WARP_CHECK(actual == hidden); return; }
    const auto white = fo4hdr::QuantizeR10Unorm(fo4hdr::EncodeSt2084Pq(203.0));
    for (UINT channel = 0; channel < 3; ++channel)
        FO4HDR_WARP_CHECK(CodeDistance(
            static_cast<std::uint16_t>((actual >> (10 * channel)) & 1023), white) <= 1);
}

struct PresentationOverlayReadback
{
    Texture& output;
    std::array<UINT, 37 * 19>& hidden;
    bool remember, visible;
};

void ReadPresentationOverlay(Device& device, const PresentationOverlayReadback& test)
{
    device.context->CopyResource(test.output.staging.Get(), test.output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(test.output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read overlay presence test");
    for (UINT y = 0; y < 19; ++y) {
        const auto* row = reinterpret_cast<const UINT*>(
            static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < 37; ++x)
            CheckPresentationOverlayPixel(row[x], test.hidden[y * 37 + x], test.remember, test.visible);
    }
    device.context->Unmap(test.output.staging.Get(), 0);
}

void VerifyDirectPresentationOverlay(Device& device, const DirectPresentationInputs& test)
{
    const auto& [scene, layer, output, shader] = test;
    constexpr UINT width=37, height=19;
    std::array<Float4, width*height> pixels{};
    const auto hiddenConstants = CreateConstantBuffer(*device.device.Get(),
        CompositionConstants{width,height,3,203,203,1000,0,0,0});
    const auto shownConstants = CreateConstantBuffer(*device.device.Get(),
        CompositionConstants{width,height,3,203,203,1000,0,0,1});
    std::array<UINT, width * height> hiddenPixels{};
    DrawPresentation(device, { shader, hiddenConstants.Get(),
        {scene.srv.Get(),nullptr,nullptr,nullptr}, output.rtv.Get(), width, height});
    ReadPresentationOverlay(device, {output, hiddenPixels, true, false});
    pixels.fill({1,1,1,1});
    device.context->UpdateSubresource(layer.texture.Get(), 0, nullptr, pixels.data(), width*sizeof(Float4), 0);
    DrawPresentation(device, { shader, shownConstants.Get(),
        {scene.srv.Get(),nullptr,nullptr,layer.srv.Get()}, output.rtv.Get(), width, height});
    ReadPresentationOverlay(device, {output, hiddenPixels, false, true});
    // Closing the overlay must ignore retained pixels without clearing its texture.
    DrawPresentation(device, { shader, hiddenConstants.Get(),
        {scene.srv.Get(),nullptr,nullptr,layer.srv.Get()}, output.rtv.Get(), width, height});
    ReadPresentationOverlay(device, {output, hiddenPixels, false, false});
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    pixels.fill({nan,nan,nan,nan});
    device.context->UpdateSubresource(layer.texture.Get(), 0, nullptr, pixels.data(), width*sizeof(Float4), 0);
    DrawPresentation(device, { shader, hiddenConstants.Get(),
        {scene.srv.Get(),nullptr,nullptr,layer.srv.Get()}, output.rtv.Get(), width, height});
    ReadPresentationOverlay(device, {output, hiddenPixels, false, false});
}

void VerifyDirectPresentationExtremes(Device& device, Texture& scene, Texture& output,
    const PresentationShaders* shader)
{
    constexpr UINT width=37, height=19;
    std::array<Float4, width*height> pixels{};
    constexpr std::array extremes{-1.0F, 0.0F, 1.0e-7F, 0.18F, 1.0F, 65504.0F,
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()};
    for (UINT i = 0; i < pixels.size(); ++i) {
        const float value = extremes[i % extremes.size()];
        pixels[i] = {value,value,value,1};
    }
    device.context->UpdateSubresource(scene.texture.Get(),0,nullptr,pixels.data(),width*sizeof(Float4),0);
    const auto extremeConstants = CreateConstantBuffer(*device.device.Get(),
        CompositionConstants{width,height,3,203,203,1000,0,0});
    DrawPresentation(device, {shader,extremeConstants.Get(),{scene.srv.Get(),nullptr,nullptr,nullptr},
        output.rtv.Get(),width,height});
    device.context->CopyResource(output.staging.Get(),output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE extremeMap{};
    CheckHresult(device.context->Map(output.staging.Get(),0,D3D11_MAP_READ,0,&extremeMap),"Read extreme presentation values");
    for (UINT y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const UINT*>(static_cast<const std::byte*>(extremeMap.pData)+y*extremeMap.RowPitch);
        for (UINT x = 0; x < width; ++x) {
            const double input = extremes[(y*width+x)%extremes.size()];
            const double nits = std::isnan(input) ? 0.0 : std::clamp(input*203,0.0,10000.0);
            const auto expected = fo4hdr::QuantizeR10Unorm(fo4hdr::EncodeSt2084Pq(nits));
            for (UINT channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>((row[x]>>(channel*10))&1023),expected)<=1);
            FO4HDR_WARP_CHECK(row[x]>>30 == 3);
        }
    }
    device.context->Unmap(output.staging.Get(),0);
}

ComPtr<ID3D11InfoQueue> VerifyRejectedPresentationView(Device& device, Texture& output)
{
    D3D11_TEXTURE2D_DESC description{};
    output.texture->GetDesc(&description);
    FO4HDR_WARP_CHECK((description.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0);
    FO4HDR_WARP_CHECK((description.BindFlags & D3D11_BIND_RENDER_TARGET) != 0);
    ComPtr<ID3D11UnorderedAccessView> rejected;
    ComPtr<ID3D11InfoQueue> diagnostics;
    (void)device.device.As(&diagnostics);
    CheckDebugMessages(device);
    FO4HDR_WARP_CHECK(FAILED(device.device->CreateUnorderedAccessView(
        output.texture.Get(), nullptr, &rejected)));
    FO4HDR_WARP_CHECK(!rejected);
    if (diagnostics) diagnostics->ClearStoredMessages();
    return diagnostics;
}

void VerifyDirectPresentation(Device& device)
{
    constexpr UINT width = 37, height = 19;
    const auto shader = CreatePresentationShaders(*device.device.Get());
    const auto referenceShader = CreateShader(*device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);
    std::array<Float4, width * height> pixels{};
    for (UINT i = 0; i < pixels.size(); ++i)
        pixels[i] = {float(i % 17) / 4.0F, float(i % 7) / 8.0F, float(i % 13) / 4.0F, 1.0F};
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * sizeof(Float4), 0};
    auto scene = CreateSizedTexture(*device.device.Get(), { width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data});
    pixels.fill({0.06F, 0.12F, 0.18F, 0.25F});
    auto layer = CreateSizedTexture(*device.device.Get(), { width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data});
    auto reference = CreateSizedTexture(*device.device.Get(), { width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS});
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, width, height);
    auto output = GetPresentationBuffer(device, *chain.Get());
    VerifyPresentationColorSpaces(*chain.Get());
    const auto diagnostics = VerifyRejectedPresentationView(device, output);
    for (const UINT mode : {0U, 1U, 2U, 3U, 10U, 11U, 12U, 13U, 14U}) {
        for (const UINT layers : {0U, 1U}) {
            const auto constants = CreateConstantBuffer(*device.device.Get(),
                CompositionConstants{width, height, mode, 203, 203, 1000, layers, layers, layers});
            const std::array<ID3D11ShaderResourceView*, 4> inputs{scene.srv.Get(),
                layer.srv.Get(), layer.srv.Get(), layers ? layer.srv.Get() : nullptr};
            DispatchPresentation(device, { referenceShader.Get(), constants.Get(), inputs,
                reference.uav.Get(), width, height});
            DrawPresentation(device, { &shader, constants.Get(), inputs,
                output.rtv.Get(), width, height});
            CheckDirectPresentation(device, {reference, output, mode, layers});
            // D3D11 keeps buffer zero's logical identity across flip presentation.
            CheckHresult(chain->Present(0, 0), "Present retained RTV test");
        }
    }
    VerifyDirectPresentationOverlay(device, {scene, layer, output, &shader});
    VerifyDirectPresentationExtremes(device, scene, output, &shader);
    CheckDebugMessages(device);
    FO4HDR_WARP_CHECK(FAILED(chain->ResizeBuffers(2, 17, 11, DXGI_FORMAT_UNKNOWN, 0)));
    if (diagnostics) diagnostics->ClearStoredMessages();
    VerifyPresentationColorSpaces(*chain.Get());
    device.context->ClearState();
    output = {};
    CheckHresult(chain->ResizeBuffers(2, 17, 11, DXGI_FORMAT_UNKNOWN, 0), "Resize after releasing buffer references");
    output = GetPresentationBuffer(device, *chain.Get());
    VerifyPresentationColorSpaces(*chain.Get());
    D3D11_TEXTURE2D_DESC resized{};
    output.texture->GetDesc(&resized);
    FO4HDR_WARP_CHECK(resized.Width == 17 && resized.Height == 11);
    const auto constants = CreateConstantBuffer(*device.device.Get(), CompositionConstants{17, 11, 11, 203, 203, 1000, 0, 0});
    DrawPresentation(device, { &shader, constants.Get(), {}, output.rtv.Get(), 17, 11});
    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read resized chain");
    const auto* row = reinterpret_cast<const UINT*>(static_cast<const std::byte*>(mapped.pData) + 5 * mapped.RowPitch);
    FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>(row[8] & 1023),
        fo4hdr::QuantizeR10Unorm(fo4hdr::EncodeSt2084Pq({203,203,203}).red)) <= 1);
    device.context->Unmap(output.staging.Get(), 0);
    CheckHresult(chain->Present(0, 0), "Present resized chain");
    device.context->ClearState();
}

struct PresentationBenchmarkSubmission
{
    Device& device;
    Texture& output;
    Texture& scratch;
    Texture& overlay;
    const PresentationShaders* shader;
    ID3D11Buffer* constants;
    ID3D11Buffer* hiddenConstants;
    std::array<ID3D11ShaderResourceView*,4> inputs;
    bool compareHiddenOverlay;

    void SubmitOverlay(UINT arm) const
    {
        auto& context = *device.context.Get();
        constexpr float transparent[4]{};
        if (arm == 1)
            context.ClearRenderTargetView(overlay.rtv.Get(), transparent);
        DrawPresentation(device, {shader,
            arm == 0 ? hiddenConstants : constants, inputs, output.rtv.Get(), 2560, 1440});
    }

    void Submit(UINT arm) const
    {
        if (compareHiddenOverlay) { SubmitOverlay(arm); return; }
        DrawPresentation(device, {shader, constants, inputs,
            arm == 0 ? output.rtv.Get() : scratch.rtv.Get(), 2560, 1440});
        if (arm != 0)
            device.context->CopyResource(output.texture.Get(), scratch.texture.Get());
    }
};

struct PresentationBenchmarkTiming
{
    static constexpr UINT samples = 64;
    ID3D11DeviceContext& context;
    ComPtr<ID3D11Query> disjoint;
    std::array<ComPtr<ID3D11Query>, samples * 4> queries;
    std::array<std::array<double,samples>,2> cpu{}, gpu{};

    explicit PresentationBenchmarkTiming(Device& device) : context(*device.context.Get())
    {
        const D3D11_QUERY_DESC disjointDescription{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
        const D3D11_QUERY_DESC timestampDescription{D3D11_QUERY_TIMESTAMP,0};
        CheckHresult(device.device->CreateQuery(&disjointDescription,&disjoint), "Create presentation disjoint query");
        for (auto& query : queries)
            CheckHresult(device.device->CreateQuery(&timestampDescription,&query), "Create presentation timestamp");
    }

    void Read(ID3D11Query* query, void* dataPointer, UINT bytes,
        std::chrono::steady_clock::time_point deadline) const
    {
        HRESULT status{};
        while ((status = context.GetData(query,dataPointer,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Presentation query timeout");
            Sleep(1);
        }
        CheckHresult(status,"Read presentation query");
    }

    void Collect()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
        Read(disjoint.Get(),&timing,sizeof(timing),deadline);
        if (timing.Disjoint || timing.Frequency == 0) throw std::runtime_error("Presentation timestamps disjoint");
        for (UINT sample = 0; sample < samples; ++sample)
            for (UINT arm = 0; arm < 2; ++arm) {
                UINT64 start{}, end{};
                Read(queries[sample*4+arm*2].Get(),&start,sizeof(start),deadline);
                Read(queries[sample*4+arm*2+1].Get(),&end,sizeof(end),deadline);
                gpu[arm][sample] = double(end-start)*1000.0/double(timing.Frequency);
            }
    }

    void Measure(const PresentationBenchmarkSubmission& submission)
    {
        context.Begin(disjoint.Get());
        for (UINT sample = 0; sample < samples; ++sample) {
            for (UINT order = 0; order < 2; ++order) {
                const UINT arm = (sample + order) % 2;
                context.End(queries[sample*4+arm*2].Get());
                const auto start = std::chrono::steady_clock::now();
                submission.Submit(arm);
                cpu[arm][sample] = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                context.End(queries[sample*4+arm*2+1].Get());
            }
        }
        context.End(disjoint.Get());
        context.Flush();
        Collect();
    }

    const char* Label(bool compareHiddenOverlay, UINT arm) const
    {
        if (compareHiddenOverlay)
            return arm == 0 ? "hidden overlay skipped" : "transparent overlay cleared+sampled";
        return arm == 0 ? "direct" : "scratch+copy";
    }

    void Report(bool compareHiddenOverlay)
    {
        for (UINT arm = 0; arm < 2; ++arm) {
            std::sort(gpu[arm].begin(),gpu[arm].end());
            std::sort(cpu[arm].begin(),cpu[arm].end());
            std::cout << "Presentation mechanism 2560x1440 " << Label(compareHiddenOverlay, arm)
                << ": GPU p50/p95 ms=" << (gpu[arm][31]+gpu[arm][32])*0.5 << '/' << gpu[arm][60]
                << ", CPU submission p50/p95 ms=" << (cpu[arm][31]+cpu[arm][32])*0.5 << '/' << cpu[arm][60] << '\n';
        }
        std::cout << "Identical fused shader; real R10 flip-chain destination; 16 warmups/64 alternating samples. Excludes Present, native frame work and display latency; no game FPS claim.\n";
    }
};

void BenchmarkDirectPresentation(Device& device, bool compareHiddenOverlay)
{
    constexpr UINT width = 2560, height = 1440;
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, width, height);
    auto output = GetPresentationBuffer(device, *chain.Get());
    auto scratch = CreateSizedTexture(*device.device.Get(), { width, height,
        DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET});
    std::vector<std::array<std::uint16_t, 4>> pixels(width * height,
        {FloatToBinary16(0.4F), FloatToBinary16(0.2F), FloatToBinary16(0.1F), FloatToBinary16(0.5F)});
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 8, 0};
    auto scene = CreateSizedTexture(*device.device.Get(), { width, height,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data});
    const auto shader = CreatePresentationShaders(*device.device.Get());
    const auto constants = CreateConstantBuffer(*device.device.Get(), CompositionConstants{width,height,3,203,203,1000,1,1});
    const auto hiddenConstants = CreateConstantBuffer(*device.device.Get(), CompositionConstants{width,height,3,203,203,1000,1,1,0});
    auto overlay = CreateSizedTexture(*device.device.Get(), { width, height,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET});
    const std::array<ID3D11ShaderResourceView*,4> inputs{scene.srv.Get(),scene.srv.Get(),scene.srv.Get(),
        compareHiddenOverlay ? overlay.srv.Get() : scene.srv.Get()};
    auto& context = *device.context.Get();
    constexpr float transparent[4]{};
    context.ClearRenderTargetView(overlay.rtv.Get(), transparent);
    const PresentationBenchmarkSubmission submission{device, output, scratch, overlay,
        &shader, constants.Get(), hiddenConstants.Get(), inputs, compareHiddenOverlay};
    PresentationBenchmarkTiming timing(device);
    for (UINT i = 0; i < 16; ++i) { submission.Submit(0); submission.Submit(1); }
    timing.Measure(submission);
    timing.Report(compareHiddenOverlay);
    context.ClearState();
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

void CheckProxyHandoff(Device& device, Texture& proxy, Texture& realBackBuffer)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    constexpr float sdrBlack[4]{};
    const auto outputShader = CreateShader(
        *device.device.Get(), fo4hdr::generated::test::kTransferEvaluationCs);
    const auto sdrConstants = CreateConstantBuffer(
        *device.device.Get(),
        OutputConstants{kWidth, kHeight, 1.0F, 1U});
    ID3D11ShaderResourceView* proxyInput = proxy.srv.Get();

    device.context->ClearRenderTargetView(proxy.rtv.Get(), sdrBlack);
    Dispatch(*device.context.Get(), { outputShader.Get(), sdrConstants.Get(),
        &proxyInput, 1, realBackBuffer.uav.Get()});
    device.context->CopyResource(
        realBackBuffer.staging.Get(), realBackBuffer.texture.Get());
    CheckHresult(device.context->Map(realBackBuffer.staging.Get(), 0,
        D3D11_MAP_READ, 0, &mapped),
        "Map black SDR proxy handoff");
    const auto black = *static_cast<const std::uint32_t*>(mapped.pData);
    FO4HDR_WARP_CHECK((black & 0x3FFFFFFFU) == 0U);
    FO4HDR_WARP_CHECK((black >> 30U) == 3U);
    device.context->Unmap(realBackBuffer.staging.Get(), 0);

    constexpr float sdrRed[4]{0.75F, 0.25F, 0.0F, 1.0F};
    device.context->ClearRenderTargetView(proxy.rtv.Get(), sdrRed);
    Dispatch(*device.context.Get(), { outputShader.Get(), sdrConstants.Get(),
        &proxyInput, 1, realBackBuffer.uav.Get()});
    device.context->CopyResource(
        realBackBuffer.staging.Get(), realBackBuffer.texture.Get());
    CheckHresult(device.context->Map(realBackBuffer.staging.Get(), 0,
        D3D11_MAP_READ, 0, &mapped),
        "Map red SDR proxy handoff");
    const auto red = *static_cast<const std::uint32_t*>(mapped.pData);
    FO4HDR_WARP_CHECK((red & 0x3FFU) == 767U);
    FO4HDR_WARP_CHECK(((red >> 10U) & 0x3FFU) == 256U);
    FO4HDR_WARP_CHECK(((red >> 20U) & 0x3FFU) == 0U);
    FO4HDR_WARP_CHECK((red >> 30U) == 3U);
    device.context->Unmap(realBackBuffer.staging.Get(), 0);
}

void VerifyPresentationProxy(Device& device)
{
    auto history = CreateTexture(
        *device.device.Get(),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D11_BIND_RENDER_TARGET);
    auto realBackBuffer = CreateTexture(
        *device.device.Get(),
        DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
    auto proxy = CreateTexture(
        *device.device.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    const std::array<ID3D11RenderTargetView*, 2> targets{
        history.rtv.Get(), proxy.rtv.Get()};
    device.context->OMSetRenderTargets(
        static_cast<UINT>(targets.size()), targets.data(), nullptr);
    std::array<ComPtr<ID3D11RenderTargetView>, 2> bound{};
    std::array<ID3D11RenderTargetView*, 2> retained{};
    device.context->OMGetRenderTargets(
        static_cast<UINT>(retained.size()), retained.data(), nullptr);
    for (std::size_t index = 0; index < retained.size(); ++index) {
        bound[index].Attach(retained[index]);
        FO4HDR_WARP_CHECK(bound[index].Get() == targets[index]);
    }

    constexpr float hdrColor[4]{4.0F, 2.0F, 1.0F, 1.0F};
    constexpr float sdrBlack[4]{};
    device.context->ClearRenderTargetView(proxy.rtv.Get(), hdrColor);
    device.context->ClearRenderTargetView(
        realBackBuffer.rtv.Get(), sdrBlack);
    device.context->OMSetRenderTargets(0, nullptr, nullptr);

    device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(
        device.context->Map(
            proxy.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "ID3D11DeviceContext::Map(FP16 presentation proxy)");
    const auto* color = static_cast<const std::uint16_t*>(mapped.pData);
    FO4HDR_WARP_CHECK(Binary16ToFloat(color[0]) == 4.0F);
    FO4HDR_WARP_CHECK(Binary16ToFloat(color[1]) == 2.0F);
    FO4HDR_WARP_CHECK(Binary16ToFloat(color[2]) == 1.0F);
    device.context->Unmap(proxy.staging.Get(), 0);

    device.context->CopyResource(
        realBackBuffer.staging.Get(), realBackBuffer.texture.Get());
    CheckHresult(
        device.context->Map(
            realBackBuffer.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "ID3D11DeviceContext::Map(real R10 presentation target)");
    FO4HDR_WARP_CHECK(*static_cast<const std::uint32_t*>(mapped.pData) == 0U);
    device.context->Unmap(realBackBuffer.staging.Get(), 0);

    CheckProxyHandoff(device, proxy, realBackBuffer);
}

struct DitherReference
{
    std::array<double, 64> codes{};
    double idealSum{};
};
DitherReference ReadDitherReference(const D3D11_MAPPED_SUBRESOURCE& floatMapped)
{
    DitherReference result;
    auto& [gpuCodes, idealSum] = result;
    for (UINT y = 0; y < 8; ++y)
    {
        const auto* row = reinterpret_cast<const Float4*>(
            static_cast<const std::byte*>(floatMapped.pData) + y * floatMapped.RowPitch);
        for (UINT x = 0; x < 8; ++x)
        {
            const auto code = static_cast<double>(row[x].red) * 1023.0;
            FO4HDR_WARP_CHECK(std::isfinite(code) && code >= 0.0 && code <= 1023.0);
            gpuCodes[y * 8 + x] = code;
            idealSum += std::floor(code + 0.5);
        }
    }
    return result;
}

struct DitherSample
{
    bool neutral;
    double expected;
};
double CheckDitherTile(const D3D11_MAPPED_SUBRESOURCE& mapped,
    const std::array<double, 64>& gpuCodes, DitherSample sample)
{
    const auto& [neutral, expected] = sample;
    double sum{};
    for (UINT y = 0; y < 8; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < 8; ++x)
        {
            const auto red = row[x] & 1023U;
            FO4HDR_WARP_CHECK(red == ((row[x] >> 10U) & 1023U));
            FO4HDR_WARP_CHECK((neutral ? red : 0U) ==
                ((row[x] >> 20U) & 1023U));
            FO4HDR_WARP_CHECK((row[x] >> 30U) == 3U);
            FO4HDR_WARP_CHECK(std::abs(red - gpuCodes[y * 8 + x]) <=
                D3D11_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP);
            if (expected < 0.001) FO4HDR_WARP_CHECK(red == 0U);
            if (expected == 1023.0) FO4HDR_WARP_CHECK(red == 1023U);
            sum += red;
        }
    }
    return sum;
}

struct DitherErrors
{
    double referenceMeanError{};
    double ditherMeanError{};
    double encodeError{};
    double quantizerMeanError{};
    double idealDitherMeanError{};
};
void ReportDitherErrors(const DitherErrors& errors)
{
    const auto& [referenceMeanError, ditherMeanError, encodeError, quantizerMeanError, idealDitherMeanError] = errors;
    std::cout << "Output dither experiment: max tile-mean error reference="
              << referenceMeanError << " dither=" << ditherMeanError
              << " GPU_FP32_encode_vs_CPU=" << encodeError
              << " ideal_dither_mean_vs_GPU=" << idealDitherMeanError
              << " R10_dither_mean_vs_GPU=" << quantizerMeanError
              << " PQ codes; test-only, native visual benefit unverified\n";
}

void VerifyOutputDitherExperiment(Device& device)
{
    const auto shader = CreateShader(*device.device.Get(),
        fo4hdr::generated::test::kTransferEvaluationCs);
    auto output = CreateSizedTexture(*device.device.Get(), { 8, 8,
        DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_UNORDERED_ACCESS});
    auto floatOutput = CreateSizedTexture(*device.device.Get(), { 8, 8,
        DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS});
    DitherErrors errors;
    auto& [referenceMeanError, ditherMeanError, encodeError, quantizerMeanError, idealDitherMeanError] = errors;
    for (const bool neutral : {true, false})
    for (const float nits : {0.0F, 0.0001F, 0.001F, 0.01F, 0.1F, 1.0F,
             203.0F, 400.0F, 430.0F, 1000.0F, 2000.0F, 10000.0F,
             -1.0F, std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()})
    {
        std::array<Float4, 64> pixels;
        pixels.fill({nits, nits, neutral ? nits : 0.0F, 1.0F});
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), 8 * sizeof(Float4), 0};
        auto input = CreateSizedTexture(*device.device.Get(), { 8, 8,
            DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &data});
        ID3D11ShaderResourceView* source = input.srv.Get();
        const auto expected = 1023.0 * fo4hdr::EncodeSt2084Pq(nits);
        double gpuReference{};
        for (const UINT mode : {0U, 2U})
        {
            const auto constants = CreateConstantBuffer(*device.device.Get(),
                OutputConstants{8, 8, 1.0F, mode});
            Dispatch(*device.context.Get(), { shader.Get(), constants.Get(),
                &source, 1, floatOutput.uav.Get()});
            device.context->CopyResource(floatOutput.staging.Get(), floatOutput.texture.Get());
            D3D11_MAPPED_SUBRESOURCE floatMapped{};
            CheckHresult(device.context->Map(floatOutput.staging.Get(), 0,
                D3D11_MAP_READ, 0, &floatMapped), "Map FP32 dither reference");
            auto reference = ReadDitherReference(floatMapped);
            const auto& [gpuCodes, idealSum] = reference;
            device.context->Unmap(floatOutput.staging.Get(), 0);
            if (mode == 0U)
            {
                gpuReference = gpuCodes[0];
                encodeError = std::max(encodeError, std::abs(gpuReference - expected));
                FO4HDR_WARP_CHECK(std::abs(gpuReference - expected) <= 0.025);
            }
            Dispatch(*device.context.Get(), { shader.Get(), constants.Get(),
                &source, 1, output.uav.Get()});
            device.context->CopyResource(output.staging.Get(), output.texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CheckHresult(device.context->Map(output.staging.Get(), 0,
                D3D11_MAP_READ, 0, &mapped), "Map output dither experiment");
            const auto sum = CheckDitherTile(mapped, gpuCodes, {neutral, expected});
            device.context->Unmap(output.staging.Get(), 0);
            const auto error = std::abs(sum / 64.0 - expected);
            if (mode == 0U) referenceMeanError = std::max(referenceMeanError, error);
            else
            {
                ditherMeanError = std::max(ditherMeanError, error);
                const auto idealError = std::abs(idealSum / 64.0 - gpuReference);
                const auto conversionError = std::abs(sum / 64.0 - gpuReference);
                idealDitherMeanError = std::max(idealDitherMeanError, idealError);
                quantizerMeanError = std::max(quantizerMeanError, conversionError);
                // A complete 64-threshold tile has half a threshold step of
                // ideal mean error. D3D11 UNORM conversion permits 0.6 code
                // distance, adding 0.1 beyond ideal nearest rounding.
                // The small allowance also covers forcing PQ's tiny zero-nit
                // code to exact black and FP32 arithmetic in the offsets.
                constexpr double arithmeticAllowance = 0.001;
                FO4HDR_WARP_CHECK(idealError <= 1.0 / 128.0 + arithmeticAllowance);
                FO4HDR_WARP_CHECK(conversionError <= 1.0 / 128.0 +
                    D3D11_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP - 0.5 + arithmeticAllowance);
            }
        }
    }
    ReportDitherErrors(errors);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

std::array<fo4hdr::LinearRgb, kWidth> CheckPipelineComposition(
    const std::array<std::uint16_t, 4>* composed, const auto& scenePixels, bool& survivedAboveOne)
{
    std::array<fo4hdr::LinearRgb, kWidth> composedReference{};
    for (std::size_t index = 0; index < scenePixels.size(); ++index) {
        const auto expectedBt2020 = fo4hdr::ConvertBt709ToBt2020({
            Binary16ToFloat(scenePixels[index][0]),
            Binary16ToFloat(scenePixels[index][1]),
            Binary16ToFloat(scenePixels[index][2])});
        const std::array expectedChannels{
            expectedBt2020.red, expectedBt2020.green, expectedBt2020.blue};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto expectedHalf = FloatToBinary16(
                static_cast<float>(expectedChannels[channel]));
            FO4HDR_WARP_CHECK(
                CodeDistance(composed[index][channel], expectedHalf) <= 1U);
            survivedAboveOne = survivedAboveOne ||
                Binary16ToFloat(composed[index][channel]) > 1.0F;
        }
        composedReference[index] = {
            Binary16ToFloat(composed[index][0]),
            Binary16ToFloat(composed[index][1]),
            Binary16ToFloat(composed[index][2])};
    }
    return composedReference;
}

void CheckPipelineOutput(const std::uint32_t* encoded, const std::array<fo4hdr::LinearRgb, kWidth>& composedReference)
{
    for (std::size_t index = 0; index < composedReference.size(); ++index) {
        fo4hdr::LinearRgb nits{
            composedReference[index].red,
            composedReference[index].green,
            composedReference[index].blue,
        };
        nits.red *= kReferenceWhiteNits;
        nits.green *= kReferenceWhiteNits;
        nits.blue *= kReferenceWhiteNits;
        const auto expected = fo4hdr::EncodeSt2084Pq(nits);
        const std::array expectedCodes{
            fo4hdr::QuantizeR10Unorm(expected.red),
            fo4hdr::QuantizeR10Unorm(expected.green),
            fo4hdr::QuantizeR10Unorm(expected.blue),
        };
        const std::array<std::uint16_t, 3> actualCodes{
            static_cast<std::uint16_t>(encoded[index] & 0x3FFU),
            static_cast<std::uint16_t>((encoded[index] >> 10U) & 0x3FFU),
            static_cast<std::uint16_t>((encoded[index] >> 20U) & 0x3FFU),
        };
        for (std::size_t channel = 0; channel < actualCodes.size(); ++channel) {
            FO4HDR_WARP_CHECK(
                CodeDistance(actualCodes[channel], expectedCodes[channel]) <=
                1U);
        }
        FO4HDR_WARP_CHECK((encoded[index] >> 30U) == 3U);
    }
}

struct PipelineTextures
{
    Texture scene, ui, composition, output;
    PipelineTextures(Device& device, D3D11_SUBRESOURCE_DATA& sceneData, D3D11_SUBRESOURCE_DATA& uiData)
    {
        scene = CreateTexture(
            *device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,
            &sceneData);
        ui = CreateTexture(
            *device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,
            &uiData);
        composition = CreateTexture(
            *device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        output = CreateTexture(
            *device.device.Get(),
            DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_UNORDERED_ACCESS);

    }
};

void VerifyLayerTransferReference(Device& device)
{
    const auto scenePixels = MakeFp16Pixels(kSamples);
    std::array<Float4, kWidth> transparent{};
    const auto uiPixels = MakeFp16Pixels(transparent);
    D3D11_SUBRESOURCE_DATA sceneData{
        scenePixels.data(),
        static_cast<UINT>(sizeof(scenePixels)),
        0};
    D3D11_SUBRESOURCE_DATA uiData{
        uiPixels.data(),
        static_cast<UINT>(sizeof(uiPixels)),
        0};

    PipelineTextures textures(device, sceneData, uiData);
    auto& [scene, ui, composition, output] = textures;
    const auto composeShader = CreateShader(
        *device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);
    const auto outputShader = CreateShader(
        *device.device.Get(), fo4hdr::generated::test::kTransferEvaluationCs);
    const auto composeConstants = CreateConstantBuffer(
        *device.device.Get(), CompositionConstants{
            kWidth, kHeight, 1, kReferenceWhiteNits,
            kReferenceWhiteNits, 1000.0F, 0U, 0U});
    const auto outputConstants = CreateConstantBuffer(
        *device.device.Get(),
        OutputConstants{kWidth, kHeight, kReferenceWhiteNits, 0U});

    std::array<ID3D11ShaderResourceView*, 4> composeInputs{
        scene.srv.Get(), nullptr, nullptr, ui.srv.Get()};
    Dispatch(
        *device.context.Get(), {
        composeShader.Get(),
        composeConstants.Get(),
        composeInputs.data(),
        static_cast<UINT>(composeInputs.size()),
        composition.uav.Get()});

    device.context->CopyResource(
        composition.staging.Get(), composition.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mappedComposition{};
    CheckHresult(
        device.context->Map(
            composition.staging.Get(), 0, D3D11_MAP_READ, 0,
            &mappedComposition),
        "ID3D11DeviceContext::Map(FP16 composition)");
    const auto* composed =
        static_cast<const std::array<std::uint16_t, 4>*>(
            mappedComposition.pData);
    bool survivedAboveOne = false;
    const auto composedReference = CheckPipelineComposition(composed, scenePixels, survivedAboveOne);
    device.context->Unmap(composition.staging.Get(), 0);
    FO4HDR_WARP_CHECK(survivedAboveOne);

    ID3D11ShaderResourceView* outputInput = composition.srv.Get();
    Dispatch(
        *device.context.Get(), {
        outputShader.Get(),
        outputConstants.Get(),
        &outputInput,
        1,
        output.uav.Get()});
    device.context->CopyResource(output.staging.Get(), output.texture.Get());

    D3D11_MAPPED_SUBRESOURCE mappedOutput{};
    CheckHresult(
        device.context->Map(
            output.staging.Get(), 0, D3D11_MAP_READ, 0, &mappedOutput),
        "ID3D11DeviceContext::Map(R10 output)");
    const auto* encoded = static_cast<const std::uint32_t*>(mappedOutput.pData);
    CheckPipelineOutput(encoded, composedReference);
    device.context->Unmap(output.staging.Get(), 0);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

struct CalibrationPass
{
    std::uint32_t mode;
    ID3D11ShaderResourceView* scene;
    ID3D11ShaderResourceView* imgui;
    std::uint32_t nativeCaptured;
    const std::array<Float4, kWidth>& expected;
};

struct CalibrationPatterns
{
    Device& device;
    static constexpr float uiWhiteNits = 200.0F;
    static constexpr float displayPeakNits = 1000.0F;
    Texture black, gameUi, backdrop, halfWhiteImGui, composition, output;
    ComPtr<ID3D11ComputeShader> composeShader, outputShader;
    ComPtr<ID3D11Buffer> outputConstants;
    fo4hdr::LinearRgb imguiPremultiplied{};

    explicit CalibrationPatterns(Device& source) : device(source) { Initialize(); }
    void Initialize()
    {
        std::array<Float4, kWidth> zeroSamples{};
        std::array<Float4, kWidth> opaqueRedSamples{};
        std::array<Float4, kWidth> opaqueGreenSamples{};
        std::array<Float4, kWidth> halfWhiteImGuiSamples{};
        opaqueRedSamples.fill({1.0F, 0.0F, 0.0F, 1.0F});
        opaqueGreenSamples.fill({0.0F, 1.0F, 0.0F, 1.0F});
        halfWhiteImGuiSamples.fill({0.5F, 0.5F, 0.5F, 0.5F});
        const auto zeroPixels = MakeFp16Pixels(zeroSamples);
        const auto opaqueRedPixels = MakeUnorm8Pixels(opaqueRedSamples);
        const auto opaqueGreenPixels = MakeFp16Pixels(opaqueGreenSamples);
        const auto halfWhiteImGuiPixels = MakeFp16Pixels(
            halfWhiteImGuiSamples);
        D3D11_SUBRESOURCE_DATA zeroData{
            zeroPixels.data(), static_cast<UINT>(sizeof(zeroPixels)), 0};
        D3D11_SUBRESOURCE_DATA gameUiData{
            opaqueRedPixels.data(),
            static_cast<UINT>(sizeof(opaqueRedPixels)), 0};
        D3D11_SUBRESOURCE_DATA backdropData{
            opaqueGreenPixels.data(),
            static_cast<UINT>(sizeof(opaqueGreenPixels)), 0};
        D3D11_SUBRESOURCE_DATA imguiData{
            halfWhiteImGuiPixels.data(),
            static_cast<UINT>(sizeof(halfWhiteImGuiPixels)), 0};

        black = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            &zeroData);
        gameUi = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
            &gameUiData);
        backdrop = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            &backdropData);
        halfWhiteImGui = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            &imguiData);
        composition = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        output = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_UNORDERED_ACCESS);
        composeShader = CreateShader(
            *device.device.Get(), fo4hdr::generated::test::kLayerEvaluationCs);
        outputShader = CreateShader(
            *device.device.Get(), fo4hdr::generated::test::kTransferEvaluationCs);
        outputConstants = CreateConstantBuffer(
            *device.device.Get(), OutputConstants{kWidth, kHeight, 1.0F, 0U});

    }
    auto CheckComposition(const std::array<std::uint16_t, 4>* values, std::uint32_t mode,
        const std::array<Float4, kWidth>& expected)
    {
        std::array<fo4hdr::LinearRgb, kWidth> storedNits{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const std::array expectedChannels{
                expected[index].red, expected[index].green,
                expected[index].blue};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto expectedHalf = FloatToBinary16(
                    expectedChannels[channel]);
                const auto distance = CodeDistance(
                    values[index][channel], expectedHalf);
                if (distance > 2U) {
                    std::cerr << "calibration mode " << mode
                              << ": sample=" << index
                              << " channel=" << channel
                              << " actual="
                              << Binary16ToFloat(values[index][channel])
                              << " expected=" << expectedChannels[channel]
                              << " half_ulp=" << distance << '\n';
                }
                FO4HDR_WARP_CHECK(distance <= 2U);
            }
            FO4HDR_WARP_CHECK(
                values[index][3] == FloatToBinary16(1.0F));
            storedNits[index] = {
                Binary16ToFloat(values[index][0]),
                Binary16ToFloat(values[index][1]),
                Binary16ToFloat(values[index][2])};
        }
        return storedNits;
    }
    void CheckOutput(const std::uint32_t* encoded, const auto& storedNits)
    {
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto pq = fo4hdr::EncodeSt2084Pq(storedNits[index]);
            const std::array expectedCodes{
                fo4hdr::QuantizeR10Unorm(pq.red),
                fo4hdr::QuantizeR10Unorm(pq.green),
                fo4hdr::QuantizeR10Unorm(pq.blue)};
            const std::array<std::uint16_t, 3> actualCodes{
                static_cast<std::uint16_t>(encoded[index] & 0x3FFU),
                static_cast<std::uint16_t>(
                    (encoded[index] >> 10U) & 0x3FFU),
                static_cast<std::uint16_t>(
                    (encoded[index] >> 20U) & 0x3FFU)};
            for (std::size_t channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(CodeDistance(
                    actualCodes[channel], expectedCodes[channel]) <= 1U);
            FO4HDR_WARP_CHECK((encoded[index] >> 30U) == 3U);
        }
    }
    void VerifyPass(CalibrationPass pass)
    {
        const auto& [mode, scene, imgui, nativeCaptured, expected] = pass;
        const auto constants = CreateConstantBuffer(*device.device.Get(),
            CompositionConstants{kWidth, kHeight, mode, uiWhiteNits,
                kReferenceWhiteNits, displayPeakNits,
                nativeCaptured, nativeCaptured});
        std::array<ID3D11ShaderResourceView*, 4> inputs{
            scene, backdrop.srv.Get(), gameUi.srv.Get(), imgui};
        Dispatch(*device.context.Get(), { composeShader.Get(), constants.Get(),
            inputs.data(), static_cast<UINT>(inputs.size()),
            composition.uav.Get()});
        device.context->CopyResource(
            composition.staging.Get(), composition.texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(composition.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            "Map calibration composition");
        const auto* values = static_cast<const
            std::array<std::uint16_t, 4>*>(mapped.pData);
        const auto storedNits = CheckComposition(values, mode, expected);
        device.context->Unmap(composition.staging.Get(), 0);

        // Calibration is authored before the same single shipping PQ encode.
        // Comparing the R10 result with the stored FP16 nits rejects a second
        // white scale, gamut conversion, or encoding at the output boundary.
        ID3D11ShaderResourceView* outputInput = composition.srv.Get();
        Dispatch(*device.context.Get(), { outputShader.Get(),
            outputConstants.Get(), &outputInput, 1, output.uav.Get()});
        device.context->CopyResource(
            output.staging.Get(), output.texture.Get());
        CheckHresult(device.context->Map(output.staging.Get(), 0,
            D3D11_MAP_READ, 0, &mapped),
            "Map calibration HDR10 output");
        const auto* encoded = static_cast<const std::uint32_t*>(mapped.pData);
        CheckOutput(encoded, storedNits);
        device.context->Unmap(output.staging.Get(), 0);
    }
    void CheckPeakRamp()
    {
        std::array<Float4, kWidth> peakRamp{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto normalized = static_cast<float>(index) /
                static_cast<float>(kWidth - 1U);
            const auto stepped = std::floor(normalized * 31.0F + 1.0e-6F) /
                31.0F;
            const auto nits = stepped * displayPeakNits * 1.125F;
            peakRamp[index] = AddImGui({nits, nits, nits});
        }
        FO4HDR_WARP_CHECK(std::abs(
            (peakRamp.back().red -
                static_cast<float>(imguiPremultiplied.red)) * 2.0F -
            displayPeakNits * 1.125F) <= 0.001F);
        VerifyPass({10U, black.srv.Get(), halfWhiteImGui.srv.Get(), 1U,
            peakRamp});

    }
    void CheckUiPatch()
    {
        std::array<Float4, kWidth> uiPatch{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto uv = (static_cast<float>(index) + 0.5F) /
                static_cast<float>(kWidth);
            const auto nits = uv >= 0.2F && uv <= 0.8F
                ? uiWhiteNits : 0.0F;
            uiPatch[index] = AddImGui({nits, nits, nits});
        }
        VerifyPass({11U, black.srv.Get(), halfWhiteImGui.srv.Get(), 1U,
            uiPatch});

    }
    void CheckNearBlack()
    {
        constexpr std::array nearBlackLevels{
            0.0F, 0.001F, 0.005F, 0.01F, 0.02F,
            0.05F, 0.1F, 0.2F, 0.5F, 1.0F};
        std::array<Float4, kWidth> nearBlack{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto levelIndex = std::min(
                index * nearBlackLevels.size() / kWidth,
                nearBlackLevels.size() - 1U);
            const auto nits = nearBlackLevels[levelIndex];
            nearBlack[index] = AddImGui({nits, nits, nits});
        }
        VerifyPass({12U, black.srv.Get(), halfWhiteImGui.srv.Get(), 1U,
            nearBlack});

    }
    void CheckGamut()
    {
        constexpr std::array<fo4hdr::LinearRgb, 6> saturatedPatches{{
            {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0},
            {1.0, 1.0, 0.0}, {0.0, 1.0, 1.0}, {1.0, 0.0, 1.0}}};
        std::array<Float4, kWidth> gamut{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const auto patchIndex = std::min(
                index * saturatedPatches.size() / kWidth,
                saturatedPatches.size() - 1U);
            const auto patch = saturatedPatches[patchIndex];
            // With the one-row harness, y >= OutputExtent.y / 2 selects the
            // explicitly BT.709-authored row and proves its one gamut conversion.
            const auto nits = fo4hdr::ConvertBt709ToBt2020({
                patch.red * uiWhiteNits,
                patch.green * uiWhiteNits,
                patch.blue * uiWhiteNits});
            gamut[index] = AddImGui(nits);
        }
        VerifyPass({13U, black.srv.Get(), halfWhiteImGui.srv.Get(), 1U, gamut});

    }
    void CheckContainment()
    {
        constexpr std::array<Float4, kWidth> peakInputs{{
            {0.0F, 0.0F, 0.0F, 1.0F},
            {4.0F, 4.0F, 4.0F, 1.0F},
            {4.49F, 4.49F, 4.49F, 1.0F},
            {4.5F, 4.5F, 4.5F, 1.0F},
            {4.75F, 1.0F, 0.5F, 1.0F},
            {4.999F, 4.0F, 1.0F, 1.0F},
            {5.0F, 1.0F, 0.5F, 1.0F},
            {8.0F, 8.0F, 8.0F, 1.0F}}};
        const auto peakInputPixels = MakeFp16Pixels(peakInputs);
        D3D11_SUBRESOURCE_DATA peakInputData{peakInputPixels.data(),
            static_cast<UINT>(sizeof(peakInputPixels)), 0};
        auto peakScene = CreateTexture(*device.device.Get(),
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            &peakInputData);
        std::array<Float4, kWidth> containment{};
        for (std::size_t index = 0; index < kWidth; ++index) {
            const std::array input{
                Binary16ToFloat(peakInputPixels[index][0]) * uiWhiteNits,
                Binary16ToFloat(peakInputPixels[index][1]) * uiWhiteNits,
                Binary16ToFloat(peakInputPixels[index][2]) * uiWhiteNits};
            const auto maximum = std::max({input[0], input[1], input[2]});
            if (maximum >= displayPeakNits * (1.0F - 1.0e-5F))
                containment[index] = {displayPeakNits, 0.0F, 0.0F, 1.0F};
            else if (maximum >= displayPeakNits * 0.9F)
                containment[index] = {
                    displayPeakNits, displayPeakNits, 0.0F, 1.0F};
            else
                containment[index] = {input[0], input[1], input[2], 1.0F};
        }
        FO4HDR_WARP_CHECK(containment[3].red == displayPeakNits &&
            containment[3].green == displayPeakNits &&
            containment[3].blue == 0.0F);
        FO4HDR_WARP_CHECK(containment[6].red == displayPeakNits &&
            containment[6].green == 0.0F && containment[6].blue == 0.0F);
        VerifyPass({14U, peakScene.srv.Get(), black.srv.Get(), 0U,
            containment});
    }
    Float4 AddImGui(fo4hdr::LinearRgb base) const
    {
        return Float4{
            static_cast<float>(imguiPremultiplied.red + base.red * 0.5),
            static_cast<float>(imguiPremultiplied.green + base.green * 0.5),
            static_cast<float>(imguiPremultiplied.blue + base.blue * 0.5),
            1.0F};
    }
    void Run()
    {
        // Full-screen calibration suppresses native UI layers while retaining
        // the plugin ImGui layer at independently scaled UI white.
        imguiPremultiplied = fo4hdr::ConvertBt709ToBt2020(
            {0.5 * uiWhiteNits, 0.5 * uiWhiteNits, 0.5 * uiWhiteNits});
        CheckPeakRamp();
        CheckUiPatch();
        CheckNearBlack();
        CheckGamut();
        CheckContainment();
    }
};

void VerifyCalibrationPatterns(Device& device)
{
    CalibrationPatterns patterns(device);
    patterns.Run();
}

}  // namespace fo4hdr::tests::gpu
