// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "d3d11/BorrowedFullscreenState.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "RendererLifetime.hpp"
#include "LiveSettings.hpp"
#include <stdexcept>

namespace fo4hdr::tests::gpu {
namespace {
struct PresentationHookScope
{
    ID3D11DeviceContext* context;
    ContextRecord& record{g_contexts.front()};
    DrawObservation observation{record};
    PsSetShader shader{record.nextPsSetShader};
    OmSetRenderTargets targets{record.nextOmSetRenderTargets};
    Draw draw{record.nextDraw};
    PresentationResources presentation{g_presentation};
    SceneResources scene{g_scene};
    RendererDevice renderer{g_rendererDevice};
    PluginConfiguration configuration{g_configuration};
    bool healthy{g_referenceHealthy.load()}, gameplay{g_gameplayReadyApplied.load()};

    explicit PresentationHookScope(Device& device) : context(device.context.Get())
    {
        if (!record.identity.Publish(context, 701)) throw std::runtime_error("Context fixture already owned");
        auto** table = *reinterpret_cast<void***>(context);
        record.nextPsSetShader = reinterpret_cast<PsSetShader>(table[kContextPsSetShader]);
        record.nextOmSetRenderTargets = reinterpret_cast<OmSetRenderTargets>(table[kContextOmSetRenderTargets]);
        record.nextDraw = reinterpret_cast<Draw>(table[kContextDraw]);
        static_cast<DrawObservation&>(record) = {};
        record.immediate = true;
        record.generation = 701;
        g_rendererDevice.activeDevice = device.device.Get();
        g_rendererDevice.activeContext = context;
        g_configuration.enabled = true;
        g_configuration.productionRequested = true;
        g_referenceHealthy = true;
        g_gameplayReadyApplied = true;
    }
    ~PresentationHookScope()
    {
        context->ClearState();
        (void)record.identity.Retire(context, 701);
        static_cast<DrawObservation&>(record) = observation;
        record.nextPsSetShader = shader;
        record.nextOmSetRenderTargets = targets;
        record.nextDraw = draw;
        g_presentation = presentation;
        g_scene = scene;
        g_rendererDevice = renderer;
        g_configuration = configuration;
        g_referenceHealthy = healthy;
        g_gameplayReadyApplied = gameplay;
    }
};
}

void VerifyPresentationContext(Device& device)
{
    PresentationHookScope scope(device);
    auto scene = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto proxy = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto output = CreateTexture(*device.device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_RENDER_TARGET);
    g_presentation = {};
    g_presentation.presentationBackBuffer = output.texture;
    g_presentation.presentationProxy = proxy.texture;
    g_presentation.presentationProxyRtv = proxy.rtv;
    g_presentation.presentationProxySrv = proxy.srv;
    g_scene.currentScene = scene.srv;
    g_scene.currentSceneDomain = LinearColorDomain::linearBt709Relative;
    auto* target = output.rtv.Get();
    HookOmSetRenderTargets(device.context.Get(), 1, &target, nullptr);
    ComPtr<ID3D11RenderTargetView> redirected;
    device.context->OMGetRenderTargets(1, &redirected, nullptr);
    FO4HDR_WARP_CHECK(redirected.Get() == proxy.rtv.Get());
    const auto observed = static_cast<DrawObservation&>(scope.record);
    const auto shaders = CreatePresentationShaders(*device.device.Get());
    const auto constants = CreateConstantBuffer(*device.device.Get(),
        CompositionConstants{kWidth, kHeight, 0, 1, 203, 1000, 0, 0, 0});
    constexpr float white[4]{1, 1, 1, 1}, black[4]{};
    device.context->ClearRenderTargetView(scene.rtv.Get(), white);
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1), "Get owned presentation context1");
    scope.record.computeQueries.Begin(&scope);
    { ScopedPresentationContext blocked(device.context.Get()); FO4HDR_WARP_CHECK(!blocked.Ready()); }
    scope.record.computeQueries.End(&scope);
    {
        ScopedPresentationContext owned(device.context.Get());
        FO4HDR_WARP_CHECK(owned.Ready());
        { ScopedPresentationContext nested(device.context.Get()); FO4HDR_WARP_CHECK(!nested.Ready()); }
        BorrowedFullscreenState saved(device.context.Get(), context1.Get(), output.texture.Get());
        DrawPresentation(device, {&shaders, constants.Get(), {scene.srv.Get(), nullptr, nullptr, nullptr},
            output.rtv.Get(), kWidth, kHeight});
        device.context->ClearRenderTargetView(output.rtv.Get(), black);
        auto* input = scene.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        HookPsSetShader(device.context.Get(), shaders.pixel.Get(), nullptr, 0);
        HookOmSetRenderTargets(device.context.Get(), 1, &target, nullptr);
        ComPtr<ID3D11RenderTargetView> bound;
        device.context->OMGetRenderTargets(1, &bound, nullptr);
        FO4HDR_WARP_CHECK(bound.Get() == output.rtv.Get());
        HookDraw(device.context.Get(), 3, 0);
    }
    FO4HDR_WARP_CHECK(scope.record.currentPrimaryRtv == observed.currentPrimaryRtv);
    FO4HDR_WARP_CHECK(scope.record.currentSecondaryRtv == observed.currentSecondaryRtv);
    FO4HDR_WARP_CHECK(scope.record.expectedBoundShader == observed.expectedBoundShader);
    FO4HDR_WARP_CHECK(scope.record.currentPassKnown == observed.currentPassKnown);
    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read actual hook-forwarded presentation draw");
    FO4HDR_WARP_CHECK(*static_cast<const UINT*>(mapped.pData) == 0xFFFFFFFFU);
    device.context->Unmap(output.staging.Get(), 0);
    CheckDebugMessages(device);
}
} // namespace fo4hdr::tests::gpu
