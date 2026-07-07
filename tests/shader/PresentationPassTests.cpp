// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
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
    explicit TerminalImage(ID3D11Device* device)
    {
        auto description = SceneDescription(presentationWidth, presentationHeight);
        description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
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

void CheckTerminalPixel(std::uint32_t packed, const Float4& scene)
{
    const auto primaries = ConvertBt709ToBt2020({scene.red, scene.green, scene.blue});
    const auto encoded = EncodeSt2084Pq(ContainBt2020Nits({primaries.red * kSceneReferenceWhiteNits,
        primaries.green * kSceneReferenceWhiteNits, primaries.blue * kSceneReferenceWhiteNits}, presentationPeakNits));
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
        auto* oldSource = source.Get();
        context->CSSetShaderResources(0, 1, &oldSource);
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
        ComPtr<ID3D11ShaderResourceView> oldSource;
        context->CSGetShaderResources(0, 1, &oldSource);
        FO4HDR_CHECK(oldSource.Get() == source.Get());
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
        presentationPeakNits, &fixture.renderer};
    const auto initialized = pass.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration);
    FO4HDR_CHECK(initialized);
    if (!initialized) throw std::runtime_error("Controlled presentation prerequisites unavailable");
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
        FO4HDR_CHECK(pass.ExecuteCurrentScene(terminal.view.Get(), presentationScope));
        state.CheckRestored();
    }
    CheckPresentationPixels(scene, ReadTerminal(fixture.gpu, terminal.texture.Get()));
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    FO4HDR_CHECK(pass.Retire());
    FO4HDR_CHECK(fixture.renderer.Retire());
    std::cout << "Checked odd-extent selected FP16 to terminal R10, linked CS, CB range and UAV counters\n";
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

HRESULT CreateControlledChain(PixelFixture& fixture, HWND window, ComPtr<IDXGISwapChain1>& chain)
{
    ComPtr<IDXGIDevice> dxgi;
    CheckHresult(fixture.gpu.device.As(&dxgi), "DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    CheckHresult(dxgi->GetAdapter(&adapter), "DXGI adapter");
    ComPtr<IDXGIFactory2> factory;
    CheckHresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory2");
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

void ReportProbeView(UINT index, ID3D11UnorderedAccessView* view)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC unordered{};
    view->GetDesc(&unordered);
    std::cout << "Chain probe buffer=" << index << " UAV format=" << unordered.Format
              << " dimension=" << unordered.ViewDimension;
    if (unordered.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D)
        std::cout << " mip=" << unordered.Texture2D.MipSlice;
    else if (unordered.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
        std::cout << " mip=" << unordered.Texture2DArray.MipSlice;
    else std::cout << " mip=not-texture2d";
    std::cout << '\n';
}

void ProbeChainBuffers(ID3D11Device* device, IDXGISwapChain1* chain)
{
    for (UINT index = 0; index < 2; ++index)
    {
        ComPtr<ID3D11Texture2D> buffer;
        const auto acquired = chain->GetBuffer(index, IID_PPV_ARGS(&buffer));
        std::cout << "Chain probe buffer=" << index << " GetBuffer HRESULT="
                  << static_cast<std::uint32_t>(acquired) << " nonnull=" << (buffer.Get() != nullptr) << '\n';
        if (FAILED(acquired) || !buffer) continue;
        D3D11_TEXTURE2D_DESC description{};
        buffer->GetDesc(&description);
        std::cout << "Chain probe buffer=" << index << " width=" << description.Width
                  << " height=" << description.Height << " format=" << description.Format
                  << " mips=" << description.MipLevels << " array=" << description.ArraySize
                  << " samples=" << description.SampleDesc.Count << " quality=" << description.SampleDesc.Quality
                  << " usage=" << description.Usage << " bind=" << description.BindFlags
                  << " cpu=" << description.CPUAccessFlags << " misc=" << description.MiscFlags << '\n';
        ComPtr<ID3D11UnorderedAccessView> view;
        const auto created = device->CreateUnorderedAccessView(buffer.Get(), nullptr, &view);
        std::cout << "Chain probe buffer=" << index << " CreateUAV HRESULT="
                  << static_cast<std::uint32_t>(created) << " nonnull=" << (view.Get() != nullptr) << '\n';
        if (FAILED(created) || !view) continue;
        ReportProbeView(index, view.Get());
    }
}

void ProbeChainColorSpace(IDXGISwapChain1* chain)
{
    ComPtr<IDXGISwapChain3> indexed;
    const auto queried = chain->QueryInterface(IID_PPV_ARGS(&indexed));
    std::cout << "Chain probe QueryInterface3 HRESULT=" << static_cast<std::uint32_t>(queried)
              << " nonnull=" << (indexed.Get() != nullptr) << '\n';
    if (FAILED(queried) || !indexed) return;
    UINT support{};
    const auto checked = indexed->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
    std::cout << "Chain probe CheckColorSpaceSupport HRESULT=" << static_cast<std::uint32_t>(checked)
              << " support=" << support << " PRESENT="
              << ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) << '\n';
}

void VerifyQualifiedFrames(PixelFixture& fixture, PresentationPass& pass, IDXGISwapChain1* chain)
{
    const LineageScenes scenes{fixture.input, fixture.destination, AdditionalScene(fixture), {}};
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
    }
    std::cout << "Checked two sequential controlled frames through the cached logical view\n";
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
        std::cout << "Controlled HDR chain unavailable; offscreen result is independent\n";
        FO4HDR_CHECK(pass.Retire());
        FO4HDR_CHECK(fixture.renderer.Retire());
        return;
    }
    CheckControlledChain(chain.Get());
    ProbeChainBuffers(fixture.gpu.device.Get(), chain.Get());
    ProbeChainColorSpace(chain.Get());
    const auto prepared = pass.PrepareChain(chain.Get());
    std::cout << "Controlled HDR preparation HRESULT=" << static_cast<std::uint32_t>(prepared) << '\n';
    if (SUCCEEDED(prepared))
    {
        std::cout << "Controlled chain HDR application qualified; physical display unverified\n";
        VerifyQualifiedFrames(fixture, pass, chain.Get());
    }
    else
    {
        std::cout << "HDR preparation rejected; R10 candidate forwarding only, SDR creation fallback untested\n";
        const auto presented = pass.Present(chain.Get(), 0, 0, PresentationScope::unavailable);
        std::cout << "Controlled native Present interval=0 flags=0 HRESULT="
                  << static_cast<std::uint32_t>(presented) << '\n';
        FO4HDR_CHECK(SUCCEEDED(presented));
    }
    FO4HDR_CHECK(pass.Retire());
    FO4HDR_CHECK(fixture.renderer.Retire());
}

}  // namespace

void VerifyPresentationPass(const std::filesystem::path& fixtureDirectory)
{
    VerifyTerminalPresentation(fixtureDirectory);
    VerifyControlledChain(fixtureDirectory);
}

}  // namespace fo4hdr::tests::gpu
