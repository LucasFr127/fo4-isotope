// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "passes/PresentationPass.hpp"
#include "passes/ObservedDraw.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/SwapChainHooks.hpp"
#include "resources/NativePresentationProxy.hpp"
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

struct AttributedScene
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    SceneTextureHandle handle;

    AttributedScene(ID3D11Device* device, ScenePassRenderer& renderer)
    {
        const auto original = SceneDescription();
        FO4HDR_CHECK(renderer.CanPromoteNativeSceneTexture(original));
        auto promoted = original;
        promoted.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        CheckHresult(device->CreateTexture2D(&promoted, nullptr, &texture), "Native private texture");
        CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &source), "Native private SRV");
        CheckHresult(device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Native private RTV");
        NativeSceneTexture candidate;
        FO4HDR_CHECK(candidate.Prepare({device, original, 1}, {texture.Get(), source.Get(), target.Get()}));
        CheckHresult(renderer.AdoptPromotedNativeSceneTexture(candidate, handle), "Commit attributed scene");
        source.Reset();
        target.Reset();
        CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &source), "Native alias SRV");
        CheckHresult(device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Native alias RTV");
    }
};

void VerifyNativeBindings(PixelFixture& fixture, ScenePassRenderer& renderer,
    const AttributedScene& input, const AttributedScene& output)
{
    auto* source = input.source.Get();
    auto* target = output.target.Get();
    const auto& descriptor = kRenoDxFallout4Shaders[0];
    fixture.gpu.context->PSSetShaderResources(descriptor.sceneInputSlot, 1, &source);
    fixture.gpu.context->OMSetRenderTargets(1, &target, nullptr);
    const auto scenes = renderer.ResolveNativeSceneBindings(descriptor);
    FO4HDR_CHECK(renderer.Views(scenes.input).texture == input.texture.Get());
    FO4HDR_CHECK(renderer.Views(scenes.destination).texture == output.texture.Get());
    FO4HDR_CHECK(renderer.Views(renderer.BoundNativeSceneHandles().input).texture == nullptr);
    auto* unrelated = fixture.renderer.Views(fixture.input).shaderResource;
    fixture.gpu.context->PSSetShaderResources(descriptor.sceneInputSlot, 1, &unrelated);
    FO4HDR_CHECK(renderer.Views(renderer.ResolveNativeSceneBindings(descriptor).input).texture == nullptr);
    fixture.gpu.context->PSSetShaderResources(descriptor.sceneInputSlot, 1, &source);
    UINT calls{};
    const FiniteDrawInvocation invocation{
        [](ID3D11DeviceContext* context, void* argument) {
            ++*static_cast<UINT*>(argument);
            context->Draw(3, 0);
        }, &calls};
    // The authored shader is unknown; this direct descriptor call retains its
    // independent caller contract and missing tone bindings forward once.
    FO4HDR_CHECK(renderer.Execute(descriptor, scenes.input, scenes.destination, invocation) ==
        SceneDrawOutcome::forwarded);
    FO4HDR_CHECK(calls == 1);
    NativeSceneAttribution marker{1, 32, 1};
    CheckHresult(input.texture->SetPrivateData(kNativeSceneAttributionGuid, 0, nullptr), "Clear native attribution");
    FO4HDR_CHECK(!ReadNativeSceneAttribution(fixture.gpu.device.Get(), input.texture.Get(), 1, marker));
    FO4HDR_CHECK(marker == NativeSceneAttribution{});
    FO4HDR_CHECK(renderer.Views(renderer.ResolveNativeSceneBindings(descriptor).input).texture == nullptr);
}

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
    FO4HDR_CHECK(borrowed.StartPipboyScreenObservation());
    FO4HDR_CHECK(borrowed.BeginFrame());
    fixture.Bind(false);
    const AttributedScene nativeInput(fixture.gpu.device.Get(), borrowed);
    const AttributedScene nativeOutput(fixture.gpu.device.Get(), borrowed);
    VerifyNativeBindings(fixture, borrowed, nativeInput, nativeOutput);
    auto* diagnosticTarget = borrowed.Views(adopted).renderTarget;
    fixture.gpu.context->OMSetRenderTargets(1, &diagnosticTarget, nullptr);
    struct NativeDraw { UINT calls{}; } draw;
    const FiniteDrawInvocation invocation{
        [](ID3D11DeviceContext* context, void* payload) {
            ++static_cast<NativeDraw*>(payload)->calls;
            context->Draw(3, 0);
        }, &draw};
    FO4HDR_CHECK(borrowed.ExecuteObserved({}, {}, invocation) == SceneDrawOutcome::forwarded);
    FO4HDR_CHECK(draw.calls == 1);
    const auto observed = borrowed.PipboyScreenSnapshot();
    FO4HDR_CHECK(observed.requested && observed.producerDraws == 0 && observed.consumerDraws == 0);
    FO4HDR_CHECK(borrowed.StopPipboyScreenObservation());
    FO4HDR_CHECK(!borrowed.PipboyScreenSnapshot().requested);
    FO4HDR_CHECK(borrowed.StartPipboyScreenObservation());
    fixture.Bind(true);
    FO4HDR_CHECK(borrowed.Retire());
    ComPtr<ID3D11PixelShader> bound;
    fixture.gpu.context->PSGetShader(&bound, nullptr, nullptr);
    FO4HDR_CHECK(bound.Get() == fixture.sentinel.Get());
    FO4HDR_CHECK(borrowed.Views(adopted).texture == nullptr);
    FO4HDR_CHECK(borrowed.Views(nativeOutput.handle).texture == nullptr);
    FO4HDR_CHECK(borrowed.Views(borrowed.ResolveNativeSceneBindings(kRenoDxFallout4Shaders[0]).destination).texture == nullptr);
    NativeSceneAttribution retiredAttribution{};
    FO4HDR_CHECK(ReadNativeSceneAttribution(fixture.gpu.device.Get(), nativeOutput.texture.Get(),
        1, retiredAttribution));
    FO4HDR_CHECK(!borrowed.PipboyScreenSnapshot().requested);
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

ComPtr<ID3D11RenderTargetView> OriginalProxyTarget(PixelFixture& fixture, IDXGISwapChain* chain)
{
    ComPtr<ID3D11Texture2D> buffer;
    CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Proxy original buffer0");
    ComPtr<ID3D11RenderTargetView> target;
    CheckHresult(fixture.gpu.device->CreateRenderTargetView(buffer.Get(), nullptr, &target), "Proxy original RTV");
    return target;
}

ComPtr<ID3D11ShaderResourceView> ProxyAliasSource(PixelFixture& fixture, ID3D11RenderTargetView* target)
{
    ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    ComPtr<ID3D11ShaderResourceView> source;
    CheckHresult(fixture.gpu.device->CreateShaderResourceView(resource.Get(), nullptr, &source), "Proxy alias SRV");
    return source;
}

void VerifyProxyInputRetirement(PixelFixture& fixture, NativePresentationProxy& proxy,
    ID3D11RenderTargetView* target)
{
    const auto alias = ProxyAliasSource(fixture, target);
    for (const auto write : uiAliasWriters) write(fixture.gpu.context.Get(), alias.Get());
    auto* foreign = fixture.renderer.Views(fixture.input).shaderResource;
    fixture.gpu.context->PSSetShaderResources(0, 1, &foreign);
    CheckHresult(proxy.Retire() ? S_OK : E_FAIL, "Retire proxy input aliases");
    for (const auto read : uiAliasReaders)
    {
        ComPtr<ID3D11ShaderResourceView> actual;
        read(fixture.gpu.context.Get(), &actual);
        FO4HDR_CHECK(!actual);
    }
    ComPtr<ID3D11ShaderResourceView> actual;
    fixture.gpu.context->PSGetShaderResources(0, 1, &actual);
    FO4HDR_CHECK(actual.Get() == foreign);
    fixture.CheckRestored(false);
}

void VerifyProxyOutputRetirement(PixelFixture& fixture, NativePresentationProxy& proxy,
    ID3D11RenderTargetView* target)
{
    CheckHresult(target != nullptr ? S_OK : E_FAIL, "Proxy output lookup");
    const auto foreign = fixture.renderer.Views(fixture.destination).renderTarget;
    std::array<ID3D11RenderTargetView*, 2> outputs{foreign, target};
    fixture.gpu.context->OMSetRenderTargets(static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
    CheckHresult(proxy.Retire() ? S_OK : E_FAIL, "Retire proxy output alias");
    outputs = {};
    fixture.gpu.context->OMGetRenderTargets(static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
    ComPtr<ID3D11RenderTargetView> first, second;
    first.Attach(outputs[0]);
    second.Attach(outputs[1]);
    FO4HDR_CHECK(first.Get() == foreign && !second);
    fixture.CheckRestored(false);
}

void VerifyProxyLifecycle(PixelFixture& fixture, IDXGISwapChain* chain)
{
    fixture.Bind();
    NativePresentationProxy proxy;
    CheckHresult(proxy.Prepare(fixture.gpu.device.Get(), fixture.gpu.context.Get(), chain)
        ? S_OK : E_FAIL, "Prepare owned presentation proxy");
    auto original = OriginalProxyTarget(fixture, chain);
    auto alias = OriginalProxyTarget(fixture, chain);
    auto* borrowed = proxy.ResolveRenderTarget(original.Get());
    CheckHresult(borrowed != nullptr ? S_OK : E_FAIL, "Proxy original lookup");
    FO4HDR_CHECK(borrowed != nullptr && proxy.ResolveRenderTarget(alias.Get()) == borrowed);
    FO4HDR_CHECK(proxy.ResolveRenderTarget(fixture.renderer.Views(fixture.destination).renderTarget) == nullptr);
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    borrowed->GetDesc(&description);
    FO4HDR_CHECK(description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    original->GetDesc(&description);
    FO4HDR_CHECK(description.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    VerifyProxyInputRetirement(fixture, proxy, borrowed);
    borrowed = nullptr;
    FO4HDR_CHECK(proxy.ResolveRenderTarget(original.Get()) == nullptr);
    CheckHresult(proxy.Prepare(fixture.gpu.device.Get(), fixture.gpu.context.Get(), chain)
        ? S_OK : E_FAIL, "Reprepare output proxy");
    VerifyProxyOutputRetirement(fixture, proxy, proxy.ResolveRenderTarget(original.Get()));
    CheckHresult(proxy.Retire() ? S_OK : E_FAIL, "Retire before proxy resize");
    // Retire releases the owner's aliases; the caller separately drops original
    // backbuffer bindings and its own views before the actual native resize.
    auto* native = original.Get();
    fixture.gpu.context->OMSetRenderTargets(1, &native, nullptr);
    fixture.gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    native = nullptr;
    original.Reset();
    alias.Reset();
    DXGI_SWAP_CHAIN_DESC chainDescription{};
    CheckHresult(chain->GetDesc(&chainDescription), "Proxy chain descriptor");
    CheckHresult(chain->ResizeBuffers(0, chainDescription.BufferDesc.Width,
        chainDescription.BufferDesc.Height, DXGI_FORMAT_UNKNOWN, chainDescription.Flags), "Proxy retired resize");
    CheckHresult(proxy.Prepare(fixture.gpu.device.Get(), fixture.gpu.context.Get(), chain)
        ? S_OK : E_FAIL, "Prepare resized proxy");
    original = OriginalProxyTarget(fixture, chain);
    FO4HDR_CHECK(proxy.ResolveRenderTarget(original.Get()) != nullptr);
    CheckHresult(proxy.Retire() ? S_OK : E_FAIL, "Retire resized proxy");
    original.Reset();
}

}  // namespace

void VerifyNativePresentationProxy(detail::PixelFixture& fixture, IDXGISwapChain* chain)
{
    VerifyProxyLifecycle(fixture, chain);
}

void VerifyRetainedNativeSdrFrame(detail::PixelFixture& fixture, PresentationPass& pass,
    IDXGISwapChain* chain)
{
    constexpr auto scope = PresentationScope::qualifiedExclusive;
    constexpr auto codes = NativeSdrCaptureScope::completedAuthoredCodesExclusive;
    NativePresentationProxy proxy;
    CheckHresult(proxy.Prepare(fixture.gpu.device.Get(), fixture.gpu.context.Get(), chain)
        ? S_OK : E_FAIL, "Prepare retained SDR proxy");
    const auto original = OriginalProxyTarget(fixture, chain);
    auto* target = proxy.ResolveRenderTarget(original.Get());
    CheckHresult(target != nullptr ? S_OK : E_FAIL, "Resolve retained SDR proxy");
    constexpr std::array authored{64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
    fixture.gpu.context->ClearRenderTargetView(target, authored.data());
    CheckHresult(proxy.CaptureCompletedSdr(NativeSdrSourceRole::ownedProxy, fixture.renderer, codes)
        ? S_OK : E_FAIL, "Capture completed authored SDR codes");
    const auto frame = fixture.renderer.FrameGeneration();
    const auto packet = proxy.CompletedFrame();
    FO4HDR_CHECK(packet.ReadyFor(frame, &fixture.renderer));
    // The original source changes after capture; only the private snapshot is handed off.
    constexpr std::array black{0.0f, 0.0f, 0.0f, 0.0f};
    fixture.gpu.context->ClearRenderTargetView(target, black.data());
    CheckHresult(pass.PrepareNativeSdrHandoff(packet, scope) ? S_OK : E_FAIL,
        "Consume retained authored SDR codes");
    pass.PrepareNativePresent(chain, 0, scope);
    FO4HDR_CHECK(!packet.ReadyFor(frame, &fixture.renderer));
    FO4HDR_CHECK(!pass.PrepareNativeSdrHandoff(packet, scope));
    CheckHresult(proxy.Retire() ? S_OK : E_FAIL, "Retire retained SDR proxy");
}

void VerifyNativeRouting(detail::PixelFixture& fixture, const std::filesystem::path& directory)
{
    VerifyBorrowedRouting(fixture, directory);
    VerifyQueryAdapters(fixture);
    VerifyCapturedPresentBoundary();
    VerifyNativeFrameLifecycle(fixture);
    VerifyQualifiedUiCapture(fixture);
}

}  // namespace fo4hdr::tests::gpu
