// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "PresentationTiming.hpp"
#include "passes/PresentationPass.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "TestHarness.hpp"

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;

constexpr UINT presentationWidth = 9;
constexpr UINT presentationHeight = 3;
constexpr float presentationPeakNits = 400.0f;
constexpr auto presentationScope = PresentationScope::qualifiedExclusive;

std::vector<Float4> PresentationInputs()
{
    constexpr std::array<Float4, presentationWidth> row{{{0, 0, 0, 1}, {1, 1, 1, 1},
        {4, 4, 4, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1},
        {4, 0.5f, 2, 1}, {0.125f, 0.25f, 0.5f, 1}, {2, 2, 2, 1}}};
    std::vector<Float4> pixels;
    for (UINT y = 0; y < presentationHeight; ++y) pixels.insert(pixels.end(), row.begin(), row.end());
    return pixels;
}

ComPtr<ID3D11ShaderResourceView> PresentationAuxiliary(PixelFixture& fixture, Float4 value)
{
    const std::vector<Float4> pixels(presentationWidth * presentationHeight, value);
    auto description = SceneDescription(presentationWidth, presentationHeight);
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), presentationWidth * sizeof(Float4), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, &data, &texture), "Presentation auxiliary");
    CheckHresult(fixture.gpu.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Presentation auxiliary SRV");
    return view;
}

void DrawPresentationTone(PixelFixture& fixture)
{
    fixture.Bind();
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, PresentationInputs());
    const auto bloom = PresentationAuxiliary(fixture, {});
    const auto exposure = PresentationAuxiliary(fixture, {1, 0, 0, 0});
    const auto tags = PresentationAuxiliary(fixture, {0, 0, 0, 4.0f / 255.0f});
    const std::array<ID3D11ShaderResourceView*, 4> views{bloom.Get(),
        fixture.renderer.Views(fixture.input).shaderResource, exposure.Get(), tags.Get()};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    const std::array<ID3D11SamplerState*, 4> samplers{fixture.point.Get(), fixture.point.Get(),
        fixture.point.Get(), fixture.point.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const auto values = ToneConstants();
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), std::span{values}.first(5));
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
    FO4HDR_CHECK(DrawPass(fixture, 0) == SceneDrawOutcome::replaced);
    CheckSelection(fixture, {});
}

LutPixels IdentityLut()
{
    LutPixels pixels(lutSize * lutSize * lutSize);
    for (UINT z = 0; z < lutSize; ++z)
        for (UINT y = 0; y < lutSize; ++y)
            for (UINT x = 0; x < lutSize; ++x)
                pixels[LutIndex(x, y, z)] = {x / 15.0f, y / 15.0f, z / 15.0f, 1};
    return pixels;
}

void DrawPresentationGrade(PixelFixture& fixture)
{
    fixture.Bind();
    const auto lut = LutTexture(fixture.gpu.device.Get(), IdentityLut());
    const std::array<ID3D11ShaderResourceView*, 7> views{
        fixture.renderer.Views(fixture.input).shaderResource, nullptr, nullptr,
        lut.Get(), lut.Get(), lut.Get(), lut.Get()};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    const std::array<ID3D11SamplerState*, 7> samplers{fixture.point.Get(), nullptr, nullptr,
        fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const std::array<Float4, 2> values{{{}, {1, 0, 0, 0}}};
    const auto constants = ConstantBuffer(fixture.gpu.device.Get(), values);
    auto* buffer = constants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &buffer);
    FO4HDR_CHECK(DrawPass(fixture, 4) == SceneDrawOutcome::replaced);
    CheckSelection(fixture, fixture.destination);
}

void SeedPresentation(PixelFixture& fixture, const LineageScenes& scenes)
{
    fixture.input = scenes.raw;
    fixture.destination = scenes.tone;
    DrawPresentationTone(fixture);
    fixture.input = scenes.tone;
    fixture.destination = scenes.grade;
    DrawPresentationGrade(fixture);
}

struct TerminalImage
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> view;
    explicit TerminalImage(ID3D11Device* device, UINT extentWidth = presentationWidth,
        UINT bindFlags = D3D11_BIND_UNORDERED_ACCESS)
    {
        auto description = SceneDescription(extentWidth, presentationHeight);
        description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        description.BindFlags = bindFlags;
        CheckHresult(device->CreateTexture2D(&description, nullptr, &texture), "Terminal R10 texture");
        CheckHresult(device->CreateUnorderedAccessView(texture.Get(), nullptr, &view), "Terminal R10 UAV");
    }
};

std::vector<std::uint32_t> ReadTerminal(const Device& gpu, ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CheckHresult(gpu.device->CreateTexture2D(&description, nullptr, &staging), "Terminal staging");
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(description.Width) * description.Height);
    gpu.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Terminal map");
    if (!mapped.pData || mapped.RowPitch < description.Width * sizeof(std::uint32_t))
    {
        gpu.context->Unmap(staging.Get(), 0);
        throw std::runtime_error("Invalid terminal readback pitch");
    }
    for (UINT row = 0; row < description.Height; ++row)
        std::memcpy(pixels.data() + row * description.Width,
            static_cast<const char*>(mapped.pData) + row * mapped.RowPitch,
            description.Width * sizeof(std::uint32_t));
    gpu.context->Unmap(staging.Get(), 0);
    return pixels;
}

void CheckTerminalNits(std::uint32_t packed, LinearRgb light)
{
    const auto encoded = EncodeSt2084Pq(ContainBt2020Nits(light, presentationPeakNits));
    const std::array expected{QuantizeR10Unorm(encoded.red), QuantizeR10Unorm(encoded.green),
        QuantizeR10Unorm(encoded.blue)};
    for (UINT channel = 0; channel < expected.size(); ++channel)
    {
        const auto actual = static_cast<int>((packed >> (channel * 10)) & 1023);
        // FP32 shader math and the double oracle may straddle a single R10 rounding boundary.
        FO4HDR_CHECK(std::abs(actual - static_cast<int>(expected[channel])) <= 1);
    }
    FO4HDR_CHECK((packed >> 30) == 3);
}

void CheckTerminalPixel(std::uint32_t packed, const Float4& scene)
{
    const auto primaries = ConvertBt709ToBt2020({scene.red, scene.green, scene.blue});
    CheckTerminalNits(packed, {primaries.red * kSceneReferenceWhiteNits,
        primaries.green * kSceneReferenceWhiteNits, primaries.blue * kSceneReferenceWhiteNits});
}

void CheckPresentationPixels(const std::vector<Float4>& scene, const std::vector<std::uint32_t>& terminal)
{
    FO4HDR_CHECK(scene.size() == presentationWidth * presentationHeight);
    FO4HDR_CHECK(terminal.size() == scene.size());
    if (scene.size() != presentationWidth * presentationHeight || terminal.size() != scene.size())
        throw std::runtime_error("Terminal extent mismatch");
    const auto intended = PresentationInputs();
    for (std::size_t index = 0; index < scene.size(); ++index)
    {
        CheckPixel(scene[index], intended[index]);
        CheckTerminalPixel(terminal[index], scene[index]);
    }
    for (UINT row = 0; row < presentationHeight; ++row)
    {
        FO4HDR_CHECK((terminal[row * presentationWidth] & 0x3fffffffu) == 0);
        FO4HDR_CHECK(scene[row * presentationWidth + 2].red > 1.0f);
    }
}

struct CounterView
{
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> view;
    explicit CounterView(ID3D11Device* device)
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = 16 * sizeof(UINT);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = sizeof(UINT);
        CheckHresult(device->CreateBuffer(&description, nullptr, &buffer), "Counter buffer");
        D3D11_UNORDERED_ACCESS_VIEW_DESC unordered{};
        unordered.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        unordered.Buffer.NumElements = 16;
        unordered.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
        CheckHresult(device->CreateUnorderedAccessView(buffer.Get(), &unordered, &view), "Counter UAV");
    }
};

UINT ReadCounter(const Device& gpu, ID3D11UnorderedAccessView* view)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(UINT);
    description.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Buffer> buffer;
    CheckHresult(gpu.device->CreateBuffer(&description, nullptr, &buffer), "Counter copy buffer");
    gpu.context->CopyStructureCount(buffer.Get(), 0, view);
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    CheckHresult(gpu.device->CreateBuffer(&description, nullptr, &staging), "Counter staging");
    gpu.context->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Counter map");
    UINT count{};
    if (mapped.pData) std::memcpy(&count, mapped.pData, sizeof(count));
    gpu.context->Unmap(staging.Get(), 0);
    FO4HDR_CHECK(mapped.pData != nullptr);
    return count;
}

struct BorrowedComputeState
{
    PixelFixture& fixture;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> instance;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> source;
    CounterView omCounter;
    CounterView csCounter;

    BorrowedComputeState(PixelFixture& pixelFixture, const std::filesystem::path& directory) :
        fixture(pixelFixture), omCounter(fixture.gpu.device.Get()), csCounter(fixture.gpu.device.Get())
    {
        CheckHresult(fixture.gpu.context.As(&context), "Immediate Context1");
        CheckHresult(fixture.gpu.device->CreateClassLinkage(&linkage), "Compute class linkage");
        const auto bytecode = ReadShader(directory / "ScenePassCompute.dxbc");
        CheckHresult(fixture.gpu.device->CreateComputeShader(bytecode.data(), bytecode.size(),
            linkage.Get(), &shader), "Compute sentinel");
        CheckHresult(linkage->CreateClassInstance("SentinelColor", 0, 0, 0, 0, &instance), "Compute class instance");
        std::array<Float4, 32> values{};
        values[16] = sentinelColor;
        constants = ConstantBuffer(fixture.gpu.device.Get(), values);
        source = PresentationAuxiliary(fixture, sentinelColor);
    }

    void Bind()
    {
        auto* classes = instance.Get();
        context->CSSetShader(shader.Get(), &classes, 1);
        auto* buffer = constants.Get();
        const UINT first = 16;
        const UINT count = 16;
        context->CSSetConstantBuffers1(0, 1, &buffer, &first, &count);
        const std::array<ID3D11ShaderResourceView*, 3> oldSources{source.Get(), source.Get(), source.Get()};
        context->CSSetShaderResources(0, static_cast<UINT>(oldSources.size()), oldSources.data());
        auto* target = fixture.renderer.Views(fixture.destination).renderTarget;
        auto* om = omCounter.view.Get();
        const UINT omCount = 11;
        context->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr, 1, 1, &om, &omCount);
        auto* cs = csCounter.view.Get();
        const UINT csCount = 7;
        context->CSSetUnorderedAccessViews(3, 1, &cs, &csCount);
        context->SetPredication(nullptr, TRUE);
    }

    void CheckShader()
    {
        ComPtr<ID3D11ComputeShader> actual;
        std::array<ID3D11ClassInstance*, 256> classes{};
        UINT count = static_cast<UINT>(classes.size());
        context->CSGetShader(&actual, classes.data(), &count);
        FO4HDR_CHECK(actual.Get() == shader.Get());
        FO4HDR_CHECK(count == 1 && classes[0] == instance.Get());
        FO4HDR_CHECK(count <= classes.size());
        for (UINT index = 0; index < std::min(count, static_cast<UINT>(classes.size())); ++index)
            if (classes[index]) classes[index]->Release();
    }

    void CheckInputs()
    {
        ComPtr<ID3D11Buffer> actual;
        UINT first{};
        UINT count{};
        context->CSGetConstantBuffers1(0, 1, &actual, &first, &count);
        FO4HDR_CHECK(actual.Get() == constants.Get());
        FO4HDR_CHECK(first == 16 && count == 16);
        std::array<ID3D11ShaderResourceView*, 3> oldSources{};
        context->CSGetShaderResources(0, static_cast<UINT>(oldSources.size()), oldSources.data());
        for (auto* oldSource : oldSources)
        {
            FO4HDR_CHECK(oldSource == source.Get());
            if (oldSource) oldSource->Release();
        }
        ComPtr<ID3D11Predicate> predicate;
        BOOL value{};
        context->GetPredication(&predicate, &value);
        FO4HDR_CHECK(!predicate && value == TRUE);
    }

    void CheckOutputs()
    {
        std::array<ID3D11RenderTargetView*, 8> targets{};
        std::array<ID3D11UnorderedAccessView*, 8> om{};
        std::array<ID3D11UnorderedAccessView*, 8> cs{};
        ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(8, targets.data(), &depth);
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, om.data());
        context->CSGetUnorderedAccessViews(0, 8, cs.data());
        FO4HDR_CHECK(!depth);
        for (UINT index = 0; index < targets.size(); ++index)
        {
            FO4HDR_CHECK(targets[index] == (index == 0 ? fixture.renderer.Views(fixture.destination).renderTarget : nullptr));
            FO4HDR_CHECK(om[index] == (index == 1 ? omCounter.view.Get() : nullptr));
            FO4HDR_CHECK(cs[index] == (index == 3 ? csCounter.view.Get() : nullptr));
            if (targets[index]) targets[index]->Release();
            if (om[index]) om[index]->Release();
            if (cs[index]) cs[index]->Release();
        }
        FO4HDR_CHECK(ReadCounter(fixture.gpu, omCounter.view.Get()) == 11);
        FO4HDR_CHECK(ReadCounter(fixture.gpu, csCounter.view.Get()) == 7);
    }

    void BindPixelRange()
    {
        auto* buffer = constants.Get();
        const UINT first = 16;
        const UINT count = 16;
        context->PSSetConstantBuffers1(0, 1, &buffer, &first, &count);
    }

    void CheckPixelRange()
    {
        ComPtr<ID3D11Buffer> actual;
        UINT first{};
        UINT count{};
        context->PSGetConstantBuffers(0, 1, &actual);
        context->PSGetConstantBuffers1(0, 1, nullptr, &first, &count);
        FO4HDR_CHECK(actual.Get() == constants.Get());
        FO4HDR_CHECK(first == 16 && count == 16);
    }

    void CheckRestored()
    {
        CheckShader();
        CheckInputs();
        CheckOutputs();
    }
};

void InitializePresentation(PresentationPass& pass, PixelFixture& fixture)
{
    const PresentationConfiguration configuration{presentationWidth, presentationHeight,
        presentationPeakNits, &fixture.renderer, fixture.whites};
    const auto initialized = pass.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration);
    FO4HDR_CHECK(initialized);
    if (!initialized) throw std::runtime_error("Controlled presentation prerequisites unavailable");
}

void CheckRejectedTerminal(PresentationPass& pass, BorrowedComputeState& state,
    TerminalImage& terminal, PresentationScope scope)
{
    const auto before = ReadTerminal(state.fixture.gpu, terminal.texture.Get());
    FO4HDR_CHECK(std::any_of(before.begin(), before.end(),
        [](std::uint32_t pixel) { return (pixel & 0x3fffffffu) != 0; }));
    FO4HDR_CHECK(!pass.ExecuteCurrentScene(terminal.view.Get(), scope));
    state.CheckRestored();
    FO4HDR_CHECK(ReadTerminal(state.fixture.gpu, terminal.texture.Get()) == before);
}

void CheckLineageBlack(PresentationPass& pass, BorrowedComputeState& state, TerminalImage& terminal)
{
    auto& fixture = state.fixture;
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.renderer.CurrentScene()));
    CheckSelection(fixture, {});
    FO4HDR_CHECK(!pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    state.CheckRestored();
    const auto black = ReadTerminal(fixture.gpu, terminal.texture.Get());
    for (const auto pixel : black) FO4HDR_CHECK(pixel == 0xc0000000u);
    FO4HDR_CHECK(pass.Retire());
    FO4HDR_CHECK(pass.Retire());
    FO4HDR_CHECK(!pass.Active());
    FO4HDR_CHECK(!pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    const PresentationConfiguration configuration{presentationWidth, presentationHeight,
        presentationPeakNits, &fixture.renderer, fixture.whites};
    FO4HDR_CHECK(!pass.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    state.CheckRestored();
    FO4HDR_CHECK(ReadTerminal(fixture.gpu, terminal.texture.Get()) == black);
}

void VerifyTerminalRejections(PresentationPass& pass, BorrowedComputeState& state,
    TerminalImage& terminal)
{
    TerminalImage wrongExtent{state.fixture.gpu.device.Get(), presentationWidth + 1};
    TerminalImage shaderInput{state.fixture.gpu.device.Get(), presentationWidth,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE};
    const float marker[]{0.5f, 0.25f, 0.75f, 1};
    state.context->ClearUnorderedAccessViewFloat(wrongExtent.view.Get(), marker);
    state.context->ClearUnorderedAccessViewFloat(shaderInput.view.Get(), marker);
    CheckRejectedTerminal(pass, state, terminal, PresentationScope::unavailable);
    CheckRejectedTerminal(pass, state, wrongExtent, presentationScope);
    CheckRejectedTerminal(pass, state, shaderInput, presentationScope);
    CheckLineageBlack(pass, state, terminal);
}

using CodedLayer = std::array<std::uint8_t, 4>;
constexpr CodedLayer backdropCodes{16, 32, 48, 64};
constexpr CodedLayer hudCodes{64, 32, 16, 128};

ComPtr<ID3D11ShaderResourceView> CompletedLayerSource(PixelFixture& fixture, CodedLayer codes)
{
    const std::vector<CodedLayer> pixels(presentationWidth * presentationHeight, codes);
    auto description = SceneDescription(presentationWidth, presentationHeight);
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), presentationWidth * sizeof(CodedLayer), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, &data, &texture), "Completed coded layer");
    CheckHresult(fixture.gpu.device->CreateShaderResourceView(texture.Get(), nullptr, &source), "Completed layer SRV");
    return source;
}

Float4 DecodedLayer(CodedLayer codes, bool hud, float uiWhiteNits)
{
    const auto coverage = codes[3] / 255.0;
    if (coverage <= 1e-6) return {0, 0, 0, static_cast<float>(coverage)};
    const auto divisor = hud ? coverage : 1.0;
    const auto scale = (hud ? coverage : 1.0) * uiWhiteNits / kSceneReferenceWhiteNits;
    return {static_cast<float>(DecodeAuthoredSrgb(codes[0] / 255.0 / divisor) * scale),
        static_cast<float>(DecodeAuthoredSrgb(codes[1] / 255.0 / divisor) * scale),
        static_cast<float>(DecodeAuthoredSrgb(codes[2] / 255.0 / divisor) * scale),
        static_cast<float>(coverage)};
}

Float4 CompositeLayer(const Float4& behind, const Float4& layer)
{
    return {layer.red + behind.red * (1 - layer.alpha),
        layer.green + behind.green * (1 - layer.alpha),
        layer.blue + behind.blue * (1 - layer.alpha), 1};
}

void VerifyLayerTransport(PresentationPass& pass, BorrowedComputeState& state, TerminalImage& terminal,
    bool measureSubmission = false)
{
    auto& fixture = state.fixture;
    // The caller supplies completed R8 code contributions and coverage.
    const auto backdrop = CompletedLayerSource(fixture, backdropCodes);
    const auto hud = CompletedLayerSource(fixture, hudCodes);
    const auto scene = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.renderer.CurrentScene()).texture);
    state.Bind();
    FO4HDR_CHECK(pass.PrepareLayers(backdrop.Get(), hud.Get(), presentationScope));
    state.CheckRestored();
    FO4HDR_CHECK(SubmitPreparedLayers(pass, terminal.view.Get(), measureSubmission));
    state.CheckRestored();
    const auto pixels = ReadTerminal(fixture.gpu, terminal.texture.Get());
    FO4HDR_CHECK(pixels.size() == scene.size());
    if (pixels.size() != scene.size()) throw std::runtime_error("Layer composition extent mismatch");
    const auto decodedBackdrop = DecodedLayer(backdropCodes, false, fixture.whites.uiWhiteNits);
    const auto decodedHud = DecodedLayer(hudCodes, true, fixture.whites.uiWhiteNits);
    for (std::size_t index = 0; index < scene.size(); ++index)
        CheckTerminalPixel(pixels[index], CompositeLayer(
            CompositeLayer(scene[index], decodedBackdrop), decodedHud));
}

struct BlendBinding
{
    ID3D11DeviceContext* context;
    ComPtr<ID3D11BlendState> blend;
    std::array<float, 4> factors{};
    UINT mask{};

    explicit BlendBinding(ID3D11DeviceContext* ownedContext) : context(ownedContext)
    {
        context->OMGetBlendState(&blend, factors.data(), &mask);
    }
    ~BlendBinding() { Restore(); }
    void Restore() const
    {
        context->OMSetBlendState(blend.Get(), factors.data(), mask);
    }
    void CheckRestored() const
    {
        ComPtr<ID3D11BlendState> actual;
        std::array<float, 4> actualFactors{};
        UINT actualMask{};
        context->OMGetBlendState(&actual, actualFactors.data(), &actualMask);
        FO4HDR_CHECK(actual.Get() == blend.Get());
        FO4HDR_CHECK(actualFactors == factors);
        FO4HDR_CHECK(actualMask == mask);
    }
};

ComPtr<ID3D11BlendState> NativeLayerBlend(ID3D11Device* device)
{
    D3D11_BLEND_DESC description{};
    auto& target = description.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    CheckHresult(device->CreateBlendState(&description, &blend), "Native layer blend");
    return blend;
}

D3D11_TEXTURE2D_DESC NativeLayerDescription()
{
    auto description = SceneDescription(presentationWidth, presentationHeight);
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return description;
}

struct NativeLayer
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> source;

    NativeLayer() = default;
    explicit NativeLayer(ID3D11Device* device,
        D3D11_TEXTURE2D_DESC description = NativeLayerDescription())
    {
        CheckHresult(device->CreateTexture2D(&description, nullptr, &texture), "Native layer texture");
        CheckHresult(device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Native layer RTV");
        CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &source), "Native layer SRV");
    }
    void Draw(PixelFixture& fixture, Float4 straight, ID3D11BlendState* blend)
    {
        BlendBinding borrowed{fixture.gpu.context.Get()};
        fixture.Bind(true);
        const std::array color{straight};
        const auto constants = ConstantBuffer(fixture.gpu.device.Get(), color);
        auto* buffer = constants.Get();
        fixture.gpu.context->PSSetConstantBuffers(0, 1, &buffer);
        auto* output = target.Get();
        fixture.gpu.context->OMSetRenderTargets(1, &output, nullptr);
        const float transparent[4]{};
        fixture.gpu.context->ClearRenderTargetView(output, transparent);
        fixture.gpu.context->OMSetBlendState(blend, nullptr, 0xffffffffu);
        fixture.gpu.context->Draw(3, 0);
        fixture.CheckRestored(true);
        borrowed.Restore();
        borrowed.CheckRestored();
    }
};

std::vector<CodedLayer> ReadNativeLayer(const Device& gpu, ID3D11Texture2D* texture)
{
    if (!texture) return std::vector<CodedLayer>(presentationWidth * presentationHeight);
    gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    // R8 RGBA and terminal R10 each store one packed 32-bit word per pixel.
    const auto packed = ReadTerminal(gpu, texture);
    std::vector<CodedLayer> pixels(packed.size());
    for (std::size_t index = 0; index < packed.size(); ++index)
        std::memcpy(pixels[index].data(), &packed[index], sizeof(CodedLayer));
    return pixels;
}

void CheckNativeLayer(const std::vector<CodedLayer>& pixels, Float4 straight)
{
    const std::array<double, 4> channels{straight.red * straight.alpha,
        straight.green * straight.alpha, straight.blue * straight.alpha, straight.alpha};
    for (const auto& pixel : pixels)
    {
        if (straight.alpha == 0) FO4HDR_CHECK(pixel == CodedLayer{});
        if (straight.alpha == 1) FO4HDR_CHECK(pixel[3] == 255);
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
        {
            const auto expected = std::lround(channels[channel] * 255.0);
            // Native blending and UNORM storage may straddle one R8 rounding boundary.
            FO4HDR_CHECK(std::abs(static_cast<long>(pixel[channel]) - expected) <= 1);
        }
    }
}

struct NativeBlendCase
{
    Float4 backdrop;
    Float4 hud;
    bool absentHud{};
};

void VerifyNativeBlendCase(PresentationPass& pass, BorrowedComputeState& state,
    TerminalImage& terminal, const NativeBlendCase& test)
{
    auto& fixture = state.fixture;
    NativeLayer backdrop{fixture.gpu.device.Get()};
    NativeLayer hud;
    const auto blend = NativeLayerBlend(fixture.gpu.device.Get());
    backdrop.Draw(fixture, test.backdrop, blend.Get());
    if (!test.absentHud)
    {
        hud = NativeLayer{fixture.gpu.device.Get()};
        hud.Draw(fixture, test.hud, blend.Get());
    }
    const auto backdropPixels = ReadNativeLayer(fixture.gpu, backdrop.texture.Get());
    const auto hudPixels = ReadNativeLayer(fixture.gpu, hud.texture.Get());
    CheckNativeLayer(backdropPixels, test.backdrop);
    if (!test.absentHud) CheckNativeLayer(hudPixels, test.hud);
    const auto scene = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.renderer.CurrentScene()).texture);
    state.Bind();
    FO4HDR_CHECK(pass.PrepareLayers(backdrop.source.Get(), hud.source.Get(), presentationScope));
    state.CheckRestored();
    FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    state.CheckRestored();
    const auto pixels = ReadTerminal(fixture.gpu, terminal.texture.Get());
    FO4HDR_CHECK(pixels.size() == scene.size());
    if (pixels.size() != scene.size()) throw std::runtime_error("Native composition extent mismatch");
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        const auto behind = CompositeLayer(scene[index], DecodedLayer(backdropPixels[index], false,
            fixture.whites.uiWhiteNits));
        CheckTerminalPixel(pixels[index], CompositeLayer(behind, DecodedLayer(hudPixels[index], true,
            fixture.whites.uiWhiteNits)));
    }
}

struct LayerInputs
{
    ID3D11ShaderResourceView* backdrop{};
    ID3D11ShaderResourceView* hud{};
};

void CheckRejectedLayers(PresentationPass& pass, BorrowedComputeState& state,
    TerminalImage& terminal, LayerInputs inputs)
{
    const auto valid = CompletedLayerSource(state.fixture, backdropCodes);
    state.Bind();
    FO4HDR_CHECK(pass.PrepareLayers(valid.Get(), nullptr, presentationScope));
    FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    state.CheckRestored();
    const auto before = ReadTerminal(state.fixture.gpu, terminal.texture.Get());
    FO4HDR_CHECK(!pass.PrepareLayers(inputs.backdrop, inputs.hud, presentationScope));
    FO4HDR_CHECK(!pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    state.CheckRestored();
    FO4HDR_CHECK(ReadTerminal(state.fixture.gpu, terminal.texture.Get()) == before);
}

void VerifyLayerRejections(PresentationPass& pass, BorrowedComputeState& state, TerminalImage& terminal)
{
    auto& fixture = state.fixture;
    auto description = NativeLayerDescription();
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    NativeLayer wrongFormat{fixture.gpu.device.Get(), description};
    description = NativeLayerDescription();
    ++description.Width;
    NativeLayer wrongExtent{fixture.gpu.device.Get(), description};
    const auto foreign = CreateWarpDevice();
    NativeLayer wrongDevice{foreign.device.Get()};
    const auto alias = CompletedLayerSource(fixture, backdropCodes);
    const std::array<LayerInputs, 4> rejected{{{wrongFormat.source.Get(), nullptr},
        {wrongExtent.source.Get(), nullptr}, {wrongDevice.source.Get(), nullptr}, {alias.Get(), alias.Get()}}};
    for (const auto inputs : rejected) CheckRejectedLayers(pass, state, terminal, inputs);
    FO4HDR_CHECK(pass.PrepareLayers(alias.Get(), nullptr, presentationScope));
    FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    const auto before = ReadTerminal(fixture.gpu, terminal.texture.Get());
    FO4HDR_CHECK(!pass.PrepareLayers(nullptr, nullptr, PresentationScope::unavailable));
    FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    state.CheckRestored();
    FO4HDR_CHECK(ReadTerminal(fixture.gpu, terminal.texture.Get()) == before);
}

void VerifyNativeUiComposition(PresentationPass& pass, BorrowedComputeState& state, TerminalImage& terminal)
{
    BlendBinding original{state.fixture.gpu.context.Get()};
    const std::array<float, 4> factors{0.125f, 0.25f, 0.5f, 0.75f};
    state.fixture.gpu.context->OMSetBlendState(nullptr, factors.data(), 0x12345679u);
    constexpr std::array<NativeBlendCase, 4> cases{{
        {{0.25f, 0.5f, 0.75f, 0}, {0.5f, 0.25f, 0.125f, 0}, false},
        {{0.25f, 0.5f, 0.75f, 0.25f}, {0.5f, 0.25f, 0.125f, 0.5f}, false},
        {{0.25f, 0.5f, 0.75f, 0.25f}, {0.5f, 0.25f, 0.125f, 1}, false},
        {{0.25f, 0.5f, 0.75f, 1}, {}, true}}};
    for (const auto& test : cases) VerifyNativeBlendCase(pass, state, terminal, test);
    VerifyLayerRejections(pass, state, terminal);
    original.Restore();
    original.CheckRestored();
}

Float4 VideoExpected(LayerWhiteConfiguration whites)
{
    const auto expanded = ConvertBt709ToBt2020({DecodeAuthoredSrgb(0.75),
        DecodeAuthoredSrgb(0.5), DecodeAuthoredSrgb(0.25)});
    const auto ratio = static_cast<double>(whites.videoWhiteNits) / whites.uiWhiteNits;
    return {static_cast<float>(expanded.red * ratio), static_cast<float>(expanded.green * ratio),
        static_cast<float>(expanded.blue * ratio), 0.5f};
}

void VerifyVideoPresentation(PresentationPass& pass, BorrowedComputeState& state, TerminalImage& terminal)
{
    auto& fixture = state.fixture;
    const auto originalInput = fixture.input;
    const auto originalDestination = fixture.destination;
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    const std::vector<Float4> input(presentationWidth * presentationHeight, {0.25f, 0.5f, 0.75f, 0.5f});
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, input);
    const auto videoConstants = BindPostProcess(fixture);
    state.BindPixelRange();
    FO4HDR_CHECK(DrawPass(fixture, 8) == SceneDrawOutcome::replaced);
    state.CheckPixelRange();
    CheckBoundInputs(fixture, 0, videoConstants.Get());
    CheckSelection(fixture, fixture.destination);
    FO4HDR_CHECK(fixture.renderer.CurrentSceneDomain() == SceneColorDomain::bt2020UiRelative);
    const auto expected = VideoExpected(fixture.whites);
    const auto video = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto& pixel : video) CheckPixel(pixel, expected);
    fixture.input = fixture.destination;
    fixture.destination = AdditionalScene(fixture);
    const auto fxaaConstants = BindPostProcess(fixture);
    FO4HDR_CHECK(DrawPass(fixture, 7) == SceneDrawOutcome::replaced);
    CheckBoundInputs(fixture, 0, fxaaConstants.Get());
    CheckSelection(fixture, fixture.destination);
    FO4HDR_CHECK(fixture.renderer.CurrentSceneDomain() == SceneColorDomain::bt2020UiRelative);
    const auto filtered = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto& pixel : filtered) CheckPixel(pixel, expected);
    state.Bind();
    FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
    state.CheckRestored();
    const auto pixels = ReadTerminal(fixture.gpu, terminal.texture.Get());
    FO4HDR_CHECK(pixels.size() == filtered.size());
    if (pixels.size() != filtered.size()) throw std::runtime_error("Video composition extent mismatch");
    // The ratio-adjusted FP16 transport is relative to the configured UI white.
    for (std::size_t index = 0; index < filtered.size(); ++index)
        CheckTerminalNits(pixels[index], {filtered[index].red * fixture.whites.uiWhiteNits,
            filtered[index].green * fixture.whites.uiWhiteNits, filtered[index].blue * fixture.whites.uiWhiteNits});
    fixture.input = originalInput;
    fixture.destination = originalDestination;
}

void VerifyTerminalPresentation(const std::filesystem::path& directory)
{
    PixelFixture fixture{directory, presentationWidth, presentationHeight};
    const LineageScenes scenes{fixture.input, fixture.destination, AdditionalScene(fixture), {}};
    PresentationPass pass;
    InitializePresentation(pass, fixture);
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    SeedPresentation(fixture, scenes);
    const auto selected = fixture.renderer.Views(fixture.renderer.CurrentScene()).texture;
    FO4HDR_CHECK(selected == fixture.renderer.Views(scenes.grade).texture);
    if (!selected) throw std::runtime_error("Presentation has no selected scene");
    const auto scene = ReadScene(fixture.gpu, selected);
    TerminalImage terminal{fixture.gpu.device.Get()};
    {
        BorrowedComputeState state{fixture, directory};
        state.Bind();
        state.CheckRestored();
        MeasureEmptyLayerSubmission(pass, terminal.view.Get());
        state.CheckRestored();
        CheckPresentationPixels(scene, ReadTerminal(fixture.gpu, terminal.texture.Get()));
        VerifyLayerTransport(pass, state, terminal, true);
        VerifyNativeUiComposition(pass, state, terminal);
        VerifyVideoPresentation(pass, state, terminal);
        FO4HDR_CHECK(fixture.renderer.BeginFrame());
        SeedPresentation(fixture, scenes);
        state.Bind();
        FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
        state.CheckRestored();
        CheckPresentationPixels(scene, ReadTerminal(fixture.gpu, terminal.texture.Get()));
        VerifyTerminalRejections(pass, state, terminal);
    }
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    FO4HDR_CHECK(fixture.renderer.Retire());
    std::cout << "Checked odd-extent selected FP16 to terminal R10, linked CS, CB range and UAV counters\n";
}

void VerifyColdWhiteLevels(const std::filesystem::path& directory)
{
    constexpr std::array<LayerWhiteConfiguration, 2> configurations{{{100, 300}, {300, 100}}};
    for (const auto whites : configurations)
    {
        PixelFixture fixture{directory, presentationWidth, presentationHeight, whites};
        const LineageScenes scenes{fixture.input, fixture.destination, AdditionalScene(fixture), {}};
        PresentationPass pass;
        InitializePresentation(pass, fixture);
        FO4HDR_CHECK(fixture.renderer.BeginFrame());
        SeedPresentation(fixture, scenes);
        const auto scene = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.renderer.CurrentScene()).texture);
        TerminalImage terminal{fixture.gpu.device.Get()};
        BorrowedComputeState state{fixture, directory};
        state.Bind();
        FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
        state.CheckRestored();
        CheckPresentationPixels(scene, ReadTerminal(fixture.gpu, terminal.texture.Get()));
        VerifyLayerTransport(pass, state, terminal);
        VerifyVideoPresentation(pass, state, terminal);
        FO4HDR_CHECK(pass.Retire());
        FO4HDR_CHECK(fixture.renderer.EndFrame());
        FO4HDR_CHECK(fixture.renderer.Retire());
    }
}

constexpr wchar_t presentationWindowClass[] = L"Fallout4HDRControlledPresentation";

LRESULT CALLBACK PresentationWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    return DefWindowProcW(window, message, wparam, lparam);
}

struct PresentationWindow
{
    HINSTANCE module{};
    ATOM registered{};
    HWND window{};
    ~PresentationWindow()
    {
        if (window && !DestroyWindow(window))
        {
            FO4HDR_CHECK(false);
            return;
        }
        if (registered) FO4HDR_CHECK(UnregisterClassW(presentationWindowClass, module) != FALSE);
    }

    bool Create()
    {
        module = GetModuleHandleW(nullptr);
        if (!module) return false;
        WNDCLASSW description{};
        description.lpfnWndProc = PresentationWindowProcedure;
        description.hInstance = module;
        description.lpszClassName = presentationWindowClass;
        registered = RegisterClassW(&description);
        if (!registered) return false;
        window = CreateWindowExW(0, presentationWindowClass, L"Controlled presentation",
            WS_OVERLAPPEDWINDOW, 0, 0, presentationWidth, presentationHeight,
            nullptr, nullptr, module, nullptr);
        return window != nullptr;
    }
};

ComPtr<IDXGIFactory2> PresentationFactory(PixelFixture& fixture)
{
    ComPtr<IDXGIDevice> dxgi;
    CheckHresult(fixture.gpu.device.As(&dxgi), "DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    CheckHresult(dxgi->GetAdapter(&adapter), "DXGI adapter");
    ComPtr<IDXGIFactory2> factory;
    CheckHresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory2");
    return factory;
}

HRESULT CreateControlledChain(PixelFixture& fixture, HWND window, ComPtr<IDXGISwapChain1>& chain)
{
    const auto factory = PresentationFactory(fixture);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = presentationWidth;
    description.Height = presentationHeight;
    description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    return factory->CreateSwapChainForHwnd(fixture.gpu.device.Get(), window, &description,
        nullptr, nullptr, &chain);
}

HRESULT CreateOriginalSdrChain(PixelFixture& fixture, HWND window, ComPtr<IDXGISwapChain>& chain)
{
    const auto factory = PresentationFactory(fixture);
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = presentationWidth;
    description.BufferDesc.Height = presentationHeight;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    return factory->CreateSwapChain(fixture.gpu.device.Get(), &description, &chain);
}

void ClearOriginalSdr(PixelFixture& fixture, IDXGISwapChain* chain)
{
    ComPtr<ID3D11Texture2D> texture;
    CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&texture)), "Original SDR buffer");
    ComPtr<ID3D11RenderTargetView> target;
    CheckHresult(fixture.gpu.device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Original SDR view");
    const float bounded[]{0.125f, 0.25f, 0.5f, 1};
    fixture.gpu.context->ClearRenderTargetView(target.Get(), bounded);
    const auto pixels = ReadTerminal(fixture.gpu, texture.Get());
    FO4HDR_CHECK(std::any_of(pixels.begin(), pixels.end(),
        [](std::uint32_t pixel) { return (pixel & 0x00ffffffu) != 0; }));
    for (const auto pixel : pixels) FO4HDR_CHECK((pixel >> 24) == 255);
}

void ForwardOriginalSdr(PixelFixture& fixture, PresentationPass& pass, HWND window)
{
    ComPtr<IDXGISwapChain> chain;
    CheckHresult(CreateOriginalSdrChain(fixture, window, chain), "Original SDR chain");
    ClearOriginalSdr(fixture, chain.Get());
    const auto rejected = pass.PrepareChain(chain.Get());
    FO4HDR_CHECK(FAILED(rejected));
    const auto presented = pass.Present(chain.Get(), 0, 0, PresentationScope::unavailable);
    std::cout << "Original SDR preparation HRESULT=" << static_cast<std::uint32_t>(rejected)
              << " native Present interval=0 flags=0 HRESULT=" << static_cast<std::uint32_t>(presented) << '\n';
    FO4HDR_CHECK(SUCCEEDED(presented));
    std::cout << "Checked caller-authored bounded SDR forwarding; game-frame handoff unverified\n";
}

void VerifyOriginalSdr(const std::filesystem::path& directory)
{
    PresentationWindow window;
    if (!window.Create()) throw std::runtime_error("Original SDR window unavailable");
    PixelFixture fixture{directory, presentationWidth, presentationHeight};
    PresentationPass pass;
    InitializePresentation(pass, fixture);
    ForwardOriginalSdr(fixture, pass, window.window);
    FO4HDR_CHECK(pass.Retire());
    FO4HDR_CHECK(fixture.renderer.Retire());
}

void CheckControlledChain(IDXGISwapChain1* chain)
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    CheckHresult(chain->GetDesc1(&description), "Controlled chain description");
    FO4HDR_CHECK(description.Width == presentationWidth && description.Height == presentationHeight);
    FO4HDR_CHECK(description.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    FO4HDR_CHECK(description.BufferCount == 2);
    FO4HDR_CHECK(description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    FO4HDR_CHECK(description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0);
    FO4HDR_CHECK(description.BufferUsage == (DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS));
}

void CheckLogicalBuffer(PixelFixture& fixture, IDXGISwapChain1* chain)
{
    ComPtr<IDXGISwapChain3> indexed;
    CheckHresult(chain->QueryInterface(IID_PPV_ARGS(&indexed)), "Controlled chain3");
    UINT support{};
    CheckHresult(indexed->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
        &support), "Controlled HDR color-space support");
    FO4HDR_CHECK((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0);
    ComPtr<ID3D11Texture2D> buffer;
    CheckHresult(indexed->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Logical controlled buffer0");
    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    FO4HDR_CHECK(description.Width == presentationWidth && description.Height == presentationHeight);
    FO4HDR_CHECK(description.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    FO4HDR_CHECK(description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0);
    FO4HDR_CHECK((description.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0);
    FO4HDR_CHECK((description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0);
    ComPtr<ID3D11Device> device;
    buffer->GetDevice(&device);
    FO4HDR_CHECK(device.Get() == fixture.gpu.device.Get());
    const auto black = ReadTerminal(fixture.gpu, buffer.Get());
    for (const auto pixel : black) FO4HDR_CHECK(pixel == 0xc0000000u);
    std::cout << "Controlled logical backbuffer=0; pre-Present black verified; post-Present pixels unverified\n";
}

void CheckIdleTest(PixelFixture& fixture, PresentationPass& pass,
    IDXGISwapChain1* chain, const LineageScenes& scenes)
{
    FO4HDR_CHECK(pass.BeginFrame(presentationScope));
    SeedPresentation(fixture, scenes);
    const auto result = pass.Present(chain, 0, DXGI_PRESENT_TEST, presentationScope);
    std::cout << "Idle native TEST HRESULT=" << static_cast<std::uint32_t>(result) << '\n';
    FO4HDR_CHECK(SUCCEEDED(result));
    CheckSelection(fixture, scenes.grade);
    const auto ordinary = pass.Present(chain, 0, 0, presentationScope);
    FO4HDR_CHECK(SUCCEEDED(ordinary));
    CheckSelection(fixture, {});
}

void VerifyQualifiedFrames(PixelFixture& fixture, PresentationPass& pass, IDXGISwapChain1* chain)
{
    const LineageScenes scenes{fixture.input, fixture.destination, AdditionalScene(fixture), {}};
    HRESULT lastPresent = S_OK;
    for (UINT frame = 1; frame <= 2; ++frame)
    {
        FO4HDR_CHECK(pass.BeginFrame(presentationScope));
        CheckSelection(fixture, {});
        CheckLogicalBuffer(fixture, chain);
        SeedPresentation(fixture, scenes);
        const auto presented = pass.Present(chain, 0, 0, presentationScope);
        std::cout << "Controlled native Present frame=" << frame << " interval=0 flags=0 HRESULT="
                  << static_cast<std::uint32_t>(presented) << '\n';
        FO4HDR_CHECK(SUCCEEDED(presented));
        CheckSelection(fixture, {});
        lastPresent = presented;
    }
    std::cout << "Checked two sequential controlled frames through the cached logical view\n";
    if (lastPresent == DXGI_STATUS_OCCLUDED) CheckIdleTest(fixture, pass, chain, scenes);
    else std::cout << "Idle TEST unexercised: no observed DXGI_STATUS_OCCLUDED\n";
}

void CheckSdrCodes(const std::vector<std::uint32_t>& pixels, CodedLayer codes)
{
    FO4HDR_CHECK(pixels.size() == presentationWidth * presentationHeight);
    for (const auto pixel : pixels)
    {
        for (UINT channel = 0; channel < 3; ++channel)
        {
            const auto expected = std::lround(codes[channel] * 1023.0 / 255.0);
            const auto actual = static_cast<long>((pixel >> (channel * 10)) & 1023);
            // The FP16 code transport may straddle one terminal rounding boundary.
            FO4HDR_CHECK(std::abs(actual - expected) <= 1);
        }
        FO4HDR_CHECK((pixel >> 30) == 3);
    }
}

void VerifySdrHandoff(PixelFixture& fixture, PresentationPass& pass,
    IDXGISwapChain1* chain, const std::filesystem::path& directory)
{
    constexpr CodedLayer codes{64, 128, 192, 255};
    const auto authored = CompletedLayerSource(fixture, codes);
    BorrowedComputeState state{fixture, directory};
    FO4HDR_CHECK(!pass.PrepareSdrHandoff(authored.Get(), presentationScope));
    FO4HDR_CHECK(pass.BeginFrame(presentationScope));
    state.Bind();
    FO4HDR_CHECK(!pass.PrepareSdrHandoff(fixture.renderer.Views(fixture.input).shaderResource,
        presentationScope));
    FO4HDR_CHECK(pass.PrepareSdrHandoff(authored.Get(), presentationScope));
    state.CheckRestored();
    ComPtr<ID3D11Texture2D> buffer;
    CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "SDR handoff buffer");
    pass.PrepareNativePresent(chain, 0, presentationScope);
    state.CheckRestored();
    CheckSdrCodes(ReadTerminal(fixture.gpu, buffer.Get()), codes);
    const auto result = chain->Present(0, 0);
    pass.CompleteNativePresent(chain, result);
    CheckHresult(result, "SDR handoff Present");
    FO4HDR_CHECK(pass.BeginFrame(presentationScope));
    pass.PrepareNativePresent(chain, 0, presentationScope);
    state.CheckRestored();
    CheckSdrCodes(ReadTerminal(fixture.gpu, buffer.Get()), {});
}

void VerifyChainRetirement(PixelFixture& fixture, PresentationPass& pass,
    IDXGISwapChain1* chain, const std::filesystem::path& directory)
{
    {
        BorrowedComputeState state{fixture, directory};
        state.Bind();
        ComPtr<ID3D11Texture2D> buffer;
        CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Retirement logical buffer");
        ComPtr<ID3D11UnorderedAccessView> alias;
        CheckHresult(fixture.gpu.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &alias),
            "Retirement logical alias");
        auto* target = alias.Get();
        const UINT preserve = UINT(-1);
        state.context->CSSetUnorderedAccessViews(0, 1, &target, &preserve);
        {
            ComPtr<ID3D11UnorderedAccessView> bound;
            state.context->CSGetUnorderedAccessViews(0, 1, &bound);
            FO4HDR_CHECK(bound.Get() != nullptr && bound.Get() == alias.Get());
        }
        FO4HDR_CHECK(pass.Retire());
        FO4HDR_CHECK(pass.Retire());
        FO4HDR_CHECK(!pass.Active());
        CheckSelection(fixture, {});
        state.CheckRestored();
    }
    const auto invalid = chain->ResizeBuffers(1, presentationWidth, presentationHeight, DXGI_FORMAT_UNKNOWN, 0);
    std::cout << "Retired invalid count1 ResizeBuffers HRESULT=" << static_cast<std::uint32_t>(invalid) << '\n';
    FO4HDR_CHECK(FAILED(invalid));
    const auto resized = chain->ResizeBuffers(2, presentationWidth, presentationHeight, DXGI_FORMAT_UNKNOWN, 0);
    std::cout << "Retired legal count2 ResizeBuffers HRESULT=" << static_cast<std::uint32_t>(resized) << '\n';
    CheckHresult(resized, "Retired controlled resize");
    FO4HDR_CHECK(!pass.Active());
    FO4HDR_CHECK(!pass.BeginFrame(presentationScope));
}

void VerifyControlledChain(const std::filesystem::path& directory)
{
    PresentationWindow window;
    if (!window.Create())
    {
        std::cout << "Controlled HDR chain unavailable: hidden window creation failed\n";
        return;
    }
    PixelFixture fixture{directory, presentationWidth, presentationHeight};
    PresentationPass pass;
    InitializePresentation(pass, fixture);
    ComPtr<IDXGISwapChain1> chain;
    const auto created = CreateControlledChain(fixture, window.window, chain);
    std::cout << "Controlled flip-chain creation HRESULT=" << static_cast<std::uint32_t>(created) << '\n';
    if (FAILED(created))
    {
        chain.Reset();
        fixture.gpu.context->Flush();
        ForwardOriginalSdr(fixture, pass, window.window);
        std::cout << "Checked one original-descriptor retry after genuine HDR creation failure\n";
        FO4HDR_CHECK(pass.Retire());
        FO4HDR_CHECK(fixture.renderer.Retire());
        return;
    }
    CheckControlledChain(chain.Get());
    const auto prepared = pass.PrepareChain(chain.Get());
    std::cout << "Controlled HDR preparation HRESULT=" << static_cast<std::uint32_t>(prepared) << '\n';
    if (SUCCEEDED(prepared))
    {
        std::cout << "Controlled chain HDR application qualified; physical display unverified\n";
        VerifySdrHandoff(fixture, pass, chain.Get(), directory);
        VerifyQualifiedFrames(fixture, pass, chain.Get());
        VerifyChainRetirement(fixture, pass, chain.Get(), directory);
        std::cout << "Original-descriptor HDR-failure retry unexercised: preparation succeeded\n";
    }
    else
    {
        std::cout << "HDR preparation rejected; forwarding R10 candidate before original-descriptor retry\n";
        const auto presented = pass.Present(chain.Get(), 0, 0, PresentationScope::unavailable);
        std::cout << "Controlled native Present interval=0 flags=0 HRESULT="
                  << static_cast<std::uint32_t>(presented) << '\n';
        FO4HDR_CHECK(SUCCEEDED(presented));
        chain.Reset();
        fixture.gpu.context->Flush();
        ForwardOriginalSdr(fixture, pass, window.window);
        std::cout << "Checked one original-descriptor retry after genuine HDR preparation failure\n";
    }
    FO4HDR_CHECK(pass.Retire());
    FO4HDR_CHECK(fixture.renderer.Retire());
}

}  // namespace

void VerifyPresentationPass(const std::filesystem::path& fixtureDirectory)
{
    VerifyTerminalPresentation(fixtureDirectory);
    VerifyColdWhiteLevels(fixtureDirectory);
    VerifyControlledChain(fixtureDirectory);
    VerifyOriginalSdr(fixtureDirectory);
}

}  // namespace fo4hdr::tests::gpu
