// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "passes/PresentationPass.hpp"
#include "passes/ObservedDraw.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/SwapChainHooks.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;

void VerifyBorrowedRouting(PixelFixture& fixture, const std::filesystem::path& directory)
{
    ScenePassRenderer borrowed;
    const ScenePassConfiguration configuration{width, height, fixture.whites};
    FO4HDR_CHECK(borrowed.InitializeBorrowed(fixture.gpu.device.Get(),
        fixture.gpu.context.Get(), configuration));
    const auto original = SceneDescription();
    auto description = original;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D11Texture2D> native;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, nullptr, &native),
        "Borrowed native texture result");
    SceneTextureHandle adopted;
    CheckHresult(borrowed.AdoptSceneTexture(original, native.Get(), adopted), "Adopt native texture");
    FO4HDR_CHECK(borrowed.Views(adopted).texture == native.Get());
    const auto code = ReadShader(directory / "ScenePassPlain.dxbc");
    FO4HDR_CHECK(!borrowed.ObserveCreatedPixelShader(std::as_bytes(std::span{code}),
        fixture.plain.Get()));
    fixture.Bind(true);
    FO4HDR_CHECK(borrowed.Retire());
    ComPtr<ID3D11PixelShader> bound;
    fixture.gpu.context->PSGetShader(&bound, nullptr, nullptr);
    FO4HDR_CHECK(bound.Get() == fixture.sentinel.Get());
    FO4HDR_CHECK(borrowed.Views(adopted).texture == nullptr);
}

struct QueryAdapterEvidence
{
    ContextQueryObserver* observer{};
    ID3D11Asynchronous* scoped{};
    UINT begins{};
    UINT ends{};
    UINT lists{};
    bool beginInsideScope{};
    bool endsInsideScope{true};
    bool listInsideScope{};
};

QueryAdapterEvidence queryAdapterEvidence;

void STDMETHODCALLTYPE QueryBeginNext(ID3D11DeviceContext*, ID3D11Asynchronous* asynchronous)
{
    ++queryAdapterEvidence.begins;
    const auto& active = queryAdapterEvidence.observer->Queries().active;
    queryAdapterEvidence.beginInsideScope =
        std::find(active.begin(), active.end(), asynchronous) != active.end();
}

void STDMETHODCALLTYPE QueryEndNext(ID3D11DeviceContext*, ID3D11Asynchronous*)
{
    ++queryAdapterEvidence.ends;
    const auto& active = queryAdapterEvidence.observer->Queries().active;
    queryAdapterEvidence.endsInsideScope &=
        std::find(active.begin(), active.end(), queryAdapterEvidence.scoped) != active.end();
}

void STDMETHODCALLTYPE QueryCommandListNext(ID3D11DeviceContext*, ID3D11CommandList*, BOOL)
{
    ++queryAdapterEvidence.lists;
    queryAdapterEvidence.listInsideScope =
        queryAdapterEvidence.observer->Queries().executionDepth == 1;
}

void VerifyQueryAdapters(PixelFixture& fixture)
{
    int first{}, unmatched{};
    ContextQuerySafety policy;
    FO4HDR_CHECK(policy.CanDispatch());
    policy.Begin(&first);
    policy.End(&unmatched);
    policy.End(&first);
    FO4HDR_CHECK(!policy.CanDispatch());
    ContextQueryObserver observer;
    const ContextQueryPredecessors next{QueryBeginNext, QueryEndNext, QueryCommandListNext};
    FO4HDR_CHECK(observer.Prepare(fixture.gpu.context.Get(), 1, next, &fixture.renderer) ==
        ContextQueryPreparation::coverageUnobserved);
    ComPtr<ID3D11Query> scope, event;
    const D3D11_QUERY_DESC scopedDescription{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC eventDescription{D3D11_QUERY_EVENT, 0};
    CheckHresult(fixture.gpu.device->CreateQuery(&scopedDescription, &scope), "Scoped query fixture");
    CheckHresult(fixture.gpu.device->CreateQuery(&eventDescription, &event), "End-only query fixture");
    queryAdapterEvidence = {&observer, scope.Get()};
    observer.Begin(fixture.gpu.context.Get(), scope.Get());
    observer.End(fixture.gpu.context.Get(), event.Get());
    observer.End(fixture.gpu.context.Get(), scope.Get());
    FO4HDR_CHECK(queryAdapterEvidence.begins == 1 && queryAdapterEvidence.ends == 2);
    FO4HDR_CHECK(queryAdapterEvidence.beginInsideScope && queryAdapterEvidence.endsInsideScope);
    FO4HDR_CHECK(std::all_of(observer.Queries().active.begin(), observer.Queries().active.end(),
        [](const void* key) { return key == nullptr; }));
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11CommandList> commands;
    CheckHresult(fixture.gpu.device->CreateDeferredContext(0, &deferred), "Deferred list fixture");
    CheckHresult(deferred->FinishCommandList(FALSE, &commands), "Empty finished list fixture");
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    observer.ExecuteCommandList(fixture.gpu.context.Get(), commands.Get(), FALSE);
    FO4HDR_CHECK(queryAdapterEvidence.lists == 1 && queryAdapterEvidence.listInsideScope);
    FO4HDR_CHECK(fixture.renderer.FrameGeneration() == 0);
    FO4HDR_CHECK(!observer.CanDispatch());
    FO4HDR_CHECK(observer.Retire());
    queryAdapterEvidence = {};
}

struct CapturedPresentProbe
{
    void** vtable{};
    std::array<void*, 18> methods{};
    PresentationPass* pass;
    UINT wrappers{};
    UINT natives{};
    UINT interval{};
    UINT flags{};

    explicit CapturedPresentProbe(PresentationPass& owner) : pass(&owner)
    {
        methods[8] = reinterpret_cast<void*>(&Intercepted);
        vtable = methods.data();
    }

    static HRESULT STDMETHODCALLTYPE Native(IDXGISwapChain* chain, UINT interval, UINT flags)
    {
        auto& probe = *reinterpret_cast<CapturedPresentProbe*>(chain);
        ++probe.natives;
        probe.interval = interval;
        probe.flags = flags;
        return DXGI_STATUS_OCCLUDED;
    }

    static HRESULT STDMETHODCALLTYPE Intercepted(IDXGISwapChain* chain, UINT interval, UINT flags)
    {
        auto& probe = *reinterpret_cast<CapturedPresentProbe*>(chain);
        if (++probe.wrappers != 1) return E_FAIL;
        probe.pass->PrepareNativePresent(chain, flags, PresentationScope::unavailable);
        const auto result = Native(chain, interval, flags);
        probe.pass->CompleteNativePresent(chain, result);
        return result;
    }
};

void VerifyCapturedPresentBoundary()
{
    PresentationPass pass;
    CapturedPresentProbe probe{pass};
    static_assert(std::is_standard_layout_v<CapturedPresentProbe>);
    // Only the owned fixture's Present slot is reachable; no driver table is cloned.
    auto* chain = reinterpret_cast<IDXGISwapChain*>(&probe);
    const auto result = pass.Present(chain, 2, DXGI_PRESENT_TEST, PresentationScope::unavailable);
    FO4HDR_CHECK(result == DXGI_STATUS_OCCLUDED);
    FO4HDR_CHECK(probe.wrappers == 1 && probe.natives == 1);
    FO4HDR_CHECK(probe.interval == 2 && probe.flags == DXGI_PRESENT_TEST);
}

void VerifyNativeFrameLifecycle(PixelFixture& fixture)
{
    ScenePassRenderer borrowed;
    const ScenePassConfiguration configuration{width, height, fixture.whites};
    FO4HDR_CHECK(borrowed.InitializeBorrowed(fixture.gpu.device.Get(),
        fixture.gpu.context.Get(), configuration));
    SceneTextureHandle oldScene;
    CheckHresult(borrowed.CreateSceneTexture(SceneDescription(), nullptr, oldScene), "Resize scene fixture");
    FO4HDR_CHECK(borrowed.BeginFrame());
    const auto firstFrame = borrowed.FrameGeneration();
    FO4HDR_CHECK(CloseNativePresentFrame(borrowed, DXGI_PRESENT_TEST));
    FO4HDR_CHECK(OpenNativePresentFrame(borrowed, DXGI_PRESENT_TEST, S_OK));
    FO4HDR_CHECK(borrowed.FrameGeneration() == firstFrame);
    PresentationPass inactive;
    CapturedPresentProbe probe{inactive};
    auto* chain = reinterpret_cast<IDXGISwapChain*>(&probe);
    FO4HDR_CHECK(CloseNativePresentFrame(borrowed, DXGI_PRESENT_DO_NOT_WAIT));
    FO4HDR_CHECK(borrowed.FrameGeneration() == 0);
    const auto result = CapturedPresentProbe::Native(chain, 3, DXGI_PRESENT_DO_NOT_WAIT);
    FO4HDR_CHECK(OpenNativePresentFrame(borrowed, DXGI_PRESENT_DO_NOT_WAIT, result));
    FO4HDR_CHECK(probe.natives == 1 && probe.interval == 3 && probe.flags == DXGI_PRESENT_DO_NOT_WAIT);
    FO4HDR_CHECK(borrowed.FrameGeneration() > firstFrame);
    FO4HDR_CHECK(borrowed.RetireForResize());
    FO4HDR_CHECK(borrowed.RetireForResize());
    FO4HDR_CHECK(borrowed.Views(oldScene).texture == nullptr);
    auto invalid = configuration;
    invalid.width = 0;
    FO4HDR_CHECK(!borrowed.ReinitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), invalid));
    FO4HDR_CHECK(!borrowed.Active() && borrowed.FrameGeneration() == 0);
    FO4HDR_CHECK(borrowed.ReinitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    SceneTextureHandle freshScene;
    CheckHresult(borrowed.CreateSceneTexture(SceneDescription(), nullptr, freshScene), "Fresh resize scene fixture");
    FO4HDR_CHECK(borrowed.Views(freshScene).texture != nullptr);
    FO4HDR_CHECK(borrowed.Views(oldScene).texture == nullptr);
    FO4HDR_CHECK(borrowed.FrameGeneration() == 0);
    FO4HDR_CHECK(OpenNativePresentFrame(borrowed, 0, S_OK));
    FO4HDR_CHECK(borrowed.FrameGeneration() > firstFrame);
    FO4HDR_CHECK(borrowed.Retire());
    FO4HDR_CHECK(!borrowed.ReinitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
}

struct UiCaptureImage
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11UnorderedAccessView> terminal;

    UiCaptureImage(ID3D11Device* device, DXGI_FORMAT format, UINT binding)
    {
        auto description = SceneDescription();
        description.Format = format;
        description.BindFlags = binding;
        CheckHresult(device->CreateTexture2D(&description, nullptr, &texture), "UI capture image");
        if ((binding & D3D11_BIND_RENDER_TARGET) != 0)
            CheckHresult(device->CreateRenderTargetView(texture.Get(), nullptr, &target), "UI capture target");
        if ((binding & D3D11_BIND_SHADER_RESOURCE) != 0)
            CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &source), "UI capture source");
        if ((binding & D3D11_BIND_UNORDERED_ACCESS) != 0)
            CheckHresult(device->CreateUnorderedAccessView(texture.Get(), nullptr, &terminal), "UI capture terminal");
    }
};

struct UiCaptureDrawProbe
{
    UINT calls{};
    static void Invoke(ID3D11DeviceContext* context, void* argument)
    {
        ++static_cast<UiCaptureDrawProbe*>(argument)->calls;
        context->DrawIndexed(6, 0, 0);
    }
    static void SeedScene(ID3D11DeviceContext* context, void*) { context->Draw(3, 0); }
};

ComPtr<ID3D11Buffer> UiCaptureIndices(ID3D11Device* device)
{
    // One fullscreen triangle and one degenerate triangle exercise six indices.
    constexpr std::array<UINT, 6> indices{0, 1, 2, 0, 0, 0};
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(indices);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_INDEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{indices.data(), 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    CheckHresult(device->CreateBuffer(&description, &data, &buffer), "UI capture indices");
    return buffer;
}

void SeedUiCaptureScene(PixelFixture& fixture, ScenePassRenderer& scenes,
    const std::array<SceneTextureHandle, 3>& handles)
{
    fixture.Bind();
    FO4HDR_CHECK(scenes.BeginFrame());
    const std::vector<Float4> pixels(width * height, {1, 1, 1, 1});
    UploadScene(fixture.gpu.context.Get(), scenes.Views(handles[0]).texture, pixels);
    const auto raw = scenes.Views(handles[0]).shaderResource;
    const std::array<ID3D11ShaderResourceView*, 4> toneSources{nullptr, raw, raw, nullptr};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(toneSources.size()), toneSources.data());
    const std::array<ID3D11SamplerState*, 4> samplers{
        fixture.point.Get(), fixture.point.Get(), fixture.point.Get(), fixture.point.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    const auto values = ToneConstants();
    const auto toneConstants = ConstantBuffer(fixture.gpu.device.Get(), std::span{values}.first(5));
    auto* constants = toneConstants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &constants);
    const FiniteDrawInvocation draw{UiCaptureDrawProbe::SeedScene, nullptr};
    FO4HDR_CHECK(scenes.Execute(kRenoDxFallout4Shaders[0], handles[0], handles[1], draw) == SceneDrawOutcome::replaced);
    const auto lut = LutTexture(fixture.gpu.device.Get(), LutInputs(0));
    const std::array<ID3D11ShaderResourceView*, 7> gradeSources{
        scenes.Views(handles[1]).shaderResource, nullptr, nullptr, lut.Get(), lut.Get(), lut.Get(), lut.Get()};
    fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(gradeSources.size()), gradeSources.data());
    const std::array<ID3D11SamplerState*, 7> gradeSamplers{
        fixture.point.Get(), nullptr, nullptr, fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get()};
    fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(gradeSamplers.size()), gradeSamplers.data());
    const std::array<Float4, 2> gradeValues{{{}, {1, 0, 0, 0}}};
    const auto gradeConstants = ConstantBuffer(fixture.gpu.device.Get(), gradeValues);
    constants = gradeConstants.Get();
    fixture.gpu.context->PSSetConstantBuffers(2, 1, &constants);
    FO4HDR_CHECK(scenes.Execute(kRenoDxFallout4Shaders[4], handles[1], handles[2], draw) == SceneDrawOutcome::replaced);
    FO4HDR_CHECK(scenes.Views(scenes.CurrentScene()).texture == scenes.Views(handles[2]).texture);
}

struct UiCaptureBinding
{
    ID3D11DeviceContext* context;
    ComPtr<ID3D11BlendState> blend;
    std::array<float, 4> factors{};
    UINT mask{};

    explicit UiCaptureBinding(ID3D11DeviceContext* value) : context(value)
    {
        context->OMGetBlendState(&blend, factors.data(), &mask);
    }
    void Check(ID3D11RenderTargetView* target) const
    {
        ComPtr<ID3D11RenderTargetView> actualTarget;
        ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(1, &actualTarget, &depth);
        FO4HDR_CHECK(actualTarget.Get() == target && !depth);
        ComPtr<ID3D11BlendState> actualBlend;
        std::array<float, 4> actualFactors{};
        UINT actualMask{};
        context->OMGetBlendState(&actualBlend, actualFactors.data(), &actualMask);
        FO4HDR_CHECK(actualBlend.Get() == blend.Get());
        FO4HDR_CHECK(actualFactors == factors && actualMask == mask);
    }
};

using UiAliasWriter = void (*)(ID3D11DeviceContext*, ID3D11ShaderResourceView*);
using UiAliasReader = void (*)(ID3D11DeviceContext*, ID3D11ShaderResourceView**);
constexpr std::array<UiAliasWriter, 6> uiAliasWriters{
    [](auto* context, auto* source) { context->VSSetShaderResources(11, 1, &source); },
    [](auto* context, auto* source) { context->HSSetShaderResources(11, 1, &source); },
    [](auto* context, auto* source) { context->DSSetShaderResources(11, 1, &source); },
    [](auto* context, auto* source) { context->GSSetShaderResources(11, 1, &source); },
    [](auto* context, auto* source) { context->PSSetShaderResources(11, 1, &source); },
    [](auto* context, auto* source) { context->CSSetShaderResources(11, 1, &source); }};
constexpr std::array<UiAliasReader, 6> uiAliasReaders{
    [](auto* context, auto** source) { context->VSGetShaderResources(11, 1, source); },
    [](auto* context, auto** source) { context->HSGetShaderResources(11, 1, source); },
    [](auto* context, auto** source) { context->DSGetShaderResources(11, 1, source); },
    [](auto* context, auto** source) { context->GSGetShaderResources(11, 1, source); },
    [](auto* context, auto** source) { context->PSGetShaderResources(11, 1, source); },
    [](auto* context, auto** source) { context->CSGetShaderResources(11, 1, source); }};

ComPtr<ID3D11Texture2D> UiCaptureTexture(ID3D11ShaderResourceView* source)
{
    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    CheckHresult(resource.As(&texture), "Completed UI capture image");
    return texture;
}

void CheckUiCaptureSources(PixelFixture& fixture, const CompletedNativeUiLayers& layers)
{
    const auto backdrop = ReadScene(fixture.gpu, UiCaptureTexture(layers.Backdrop()).Get());
    for (const auto& pixel : backdrop) CheckPixel(pixel, {0.03125f, 0.0625f, 0.125f, 0.5f});
    const auto hud = UiCaptureTexture(layers.Hud());
    ComPtr<ID3D11Texture2D> readback;
    auto description = SceneDescription();
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, nullptr, &readback), "HUD readback");
    fixture.gpu.context->CopyResource(readback.Get(), hud.Get());
    D3D11_MAPPED_SUBRESOURCE mapping{};
    CheckHresult(fixture.gpu.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapping), "HUD map");
    const auto* pixel = static_cast<const std::uint8_t*>(mapping.pData);
    constexpr std::array<int, 4> expected{8, 16, 32, 128};
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
        FO4HDR_CHECK(std::abs(int(pixel[channel]) - expected[channel]) <= 1);
    fixture.gpu.context->Unmap(readback.Get(), 0);
}

void CheckUiAliasRestoration(NativeUiCapture& capture, PixelFixture& fixture,
    const CompletedNativeUiLayers& layers, std::uint64_t frame)
{
    for (const auto write : uiAliasWriters) write(fixture.gpu.context.Get(), layers.Hud());
    UiCaptureDrawProbe probe;
    FO4HDR_CHECK(capture.ExecuteQualified({frame, NativeUiShaderRole::hudComposite,
        fixture.plain.Get(), NativeUiDrawKind::indexedSix}, {UiCaptureDrawProbe::Invoke, &probe}) == SceneDrawOutcome::replaced);
    FO4HDR_CHECK(probe.calls == 1);
    for (const auto read : uiAliasReaders)
    {
        ComPtr<ID3D11ShaderResourceView> actual;
        read(fixture.gpu.context.Get(), &actual);
        FO4HDR_CHECK(actual.Get() == layers.Hud());
    }
    for (const auto write : uiAliasWriters) write(fixture.gpu.context.Get(), nullptr);
}

void VerifyQualifiedUiCapture(PixelFixture& fixture)
{
    ScenePassRenderer scenes;
    const ScenePassConfiguration configuration{width, height, fixture.whites};
    FO4HDR_CHECK(scenes.InitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    std::array<SceneTextureHandle, 3> handles;
    for (auto& handle : handles)
        CheckHresult(scenes.CreateSceneTexture(SceneDescription(), nullptr, handle), "UI selected scene");
    SeedUiCaptureScene(fixture, scenes, handles);
    NativeUiCapture capture;
    FO4HDR_CHECK(capture.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    constexpr UINT transportBindings = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    UiCaptureImage logical{fixture.gpu.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, transportBindings};
    UiCaptureImage source{fixture.gpu.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, transportBindings};
    const float transparent[4]{};
    fixture.gpu.context->ClearRenderTargetView(logical.target.Get(), transparent);
    fixture.gpu.context->ClearRenderTargetView(source.target.Get(), transparent);
    const auto indices = UiCaptureIndices(fixture.gpu.device.Get());
    fixture.Bind();
    auto* target = logical.target.Get();
    auto* input = source.source.Get();
    fixture.gpu.context->OMSetRenderTargets(1, &target, nullptr);
    fixture.gpu.context->PSSetShaderResources(0, 1, &input);
    fixture.gpu.context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    const auto frame = scenes.FrameGeneration();
    capture.AdvanceFrame(frame);
    FO4HDR_CHECK(capture.Status() == NativeUiFrameStatus::targetUnqualified);
    FO4HDR_CHECK(capture.BeginFrame({frame, target, &scenes}, NativeUiCaptureScope::qualifiedExclusive));
    UiCaptureBinding binding{fixture.gpu.context.Get()};
    UiCaptureDrawProbe probe;
    for (const auto role : {NativeUiShaderRole::gammaBackdrop, NativeUiShaderRole::hudComposite})
    {
        FO4HDR_CHECK(capture.ExecuteQualified({frame, role, fixture.plain.Get(), NativeUiDrawKind::indexedSix},
            {UiCaptureDrawProbe::Invoke, &probe}) == SceneDrawOutcome::replaced);
        binding.Check(target);
        fixture.CheckRestored(false);
    }
    FO4HDR_CHECK(probe.calls == 2);
    const auto layers = capture.CompletedLayers();
    FO4HDR_CHECK(layers.ReadyFor(frame, &scenes));
    CheckUiCaptureSources(fixture, layers);
    fixture.gpu.context->OMSetRenderTargets(1, &target, nullptr);
    CheckUiAliasRestoration(capture, fixture, layers, frame);
    PresentationPass presentation;
    FO4HDR_CHECK(presentation.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(),
        {width, height, 400, &scenes, fixture.whites}));
    UiCaptureImage terminal{fixture.gpu.device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_UNORDERED_ACCESS};
    FO4HDR_CHECK(presentation.PrepareNativeLayers(layers, PresentationScope::qualifiedExclusive));
    FO4HDR_CHECK(presentation.ExecuteCurrentScene(terminal.terminal.Get(), PresentationScope::qualifiedExclusive));
    input = layers.Backdrop();
    fixture.gpu.context->PSSetShaderResources(0, 1, &input);
    FO4HDR_CHECK(capture.ExecuteQualified({frame, NativeUiShaderRole::hudComposite,
        fixture.plain.Get(), NativeUiDrawKind::indexedSix}, {UiCaptureDrawProbe::Invoke, &probe}) == SceneDrawOutcome::forwarded);
    FO4HDR_CHECK(probe.calls == 2 && capture.Status() == NativeUiFrameStatus::failed);
    FO4HDR_CHECK(!presentation.PrepareNativeLayers(layers, PresentationScope::qualifiedExclusive));
    FO4HDR_CHECK(!presentation.ExecuteCurrentScene(terminal.terminal.Get(), PresentationScope::qualifiedExclusive));
    FO4HDR_CHECK(presentation.Retire());
    FO4HDR_CHECK(capture.Retire());
    FO4HDR_CHECK(!layers.ReadyFor(frame, &scenes));
    FO4HDR_CHECK(scenes.Retire());
    fixture.gpu.context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
}

}  // namespace

void VerifyNativeRouting(detail::PixelFixture& fixture, const std::filesystem::path& directory)
{
    VerifyBorrowedRouting(fixture, directory);
    VerifyQueryAdapters(fixture);
    VerifyCapturedPresentBoundary();
    VerifyNativeFrameLifecycle(fixture);
    VerifyQualifiedUiCapture(fixture);
}

}  // namespace fo4hdr::tests::gpu
