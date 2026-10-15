// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "hooks/ContextHooks.hpp"
#include "passes/ObservedDraw.hpp"
#include "resources/SceneResources.hpp"
#include "ShaderRegistry.hpp"
#include "PresentFrame.hpp"
#include "RendererLifetime.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "FO4Isotope/runtime/RuntimeProfile.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace fo4hdr::tests::gpu {
namespace {

struct NativeSceneInitializationFixture : SharedPresentationContextFixture
{
    Texture scene;
    ComPtr<ID3D11Buffer> indices;
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11BlendState> blendState;
    static constexpr float stale[4]{17440, 31408, 41152, 1};

    explicit NativeSceneInitializationFixture(Device& value) :
        SharedPresentationContextFixture(value)
    {
        scene = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        HudConstants data;
        data.color = {0.25F, 0.5F, 0.75F, 1};
        hudConstants = CreateConstantBuffer(*device.device.Get(), data);
        PublishGraphicsLifecycle(true, false);
        CreateNativeState();
        Bind();
    }

    ~NativeSceneInitializationFixture()
    {
        ResetShaderRegistry();
    }

    void CreateNativeState()
    {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = kWidth; texture.Height = kHeight;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_D32_FLOAT;
        texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        std::array<float, kWidth> depths{};
        depths.fill(0.5F);
        depths[1] = 1;
        const D3D11_SUBRESOURCE_DATA data{depths.data(), sizeof(depths), 0};
        CheckHresult(device.device->CreateTexture2D(&texture, &data, &depthTexture),
            "Create scene depth hole");
        CheckHresult(device.device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView),
            "Create scene depth view");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthFunc = D3D11_COMPARISON_GREATER;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        CheckHresult(device.device->CreateDepthStencilState(&depth, &depthState),
            "Create native scene depth test");
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
            D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        CheckHresult(device.device->CreateBlendState(&blend, &blendState),
            "Create native RGB scene writes");
        const std::array<std::uint16_t, 3> triangle{0, 1, 2};
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(triangle); buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA indexData{triangle.data(), 0, 0};
        CheckHresult(device.device->CreateBuffer(&buffer, &indexData, &indices),
            "Create scene triangle indices");
        CreateNativeTriangle();
    }

    void CreateNativeTriangle()
    {
        const std::array<Float4, 3> triangle{{{-1, 1, 1, 1}, {3, 1, 1, 1}, {-1, -3, 1, 1}}};
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(triangle); buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{triangle.data(), 0, 0};
        CheckHresult(device.device->CreateBuffer(&buffer, &data, &vertices),
            "Create scene far-depth vertices");
        const auto& vs = generated::test::kFullscreenStateVs;
        vertexShader = CreateVertexShader(*device.device.Get(), vs);
        const D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
            0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        CheckHresult(device.device->CreateInputLayout(
            &element, 1, vs.data(), vs.size(), &inputLayout),
            "Create scene input layout");
    }

    void Bind()
    {
        auto* context = device.context.Get();
        auto* target = scene.rtv.Get();
        context->OMSetRenderTargets(1, &target, depthView.Get());
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetBlendState(blendState.Get(), nullptr, ~0U);
        context->RSSetState(g_presentation.presentRasterizer.Get());
        const D3D11_VIEWPORT viewport{0, 0, kWidth, kHeight, 0.01F, 1};
        context->RSSetViewports(1, &viewport);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(hudShader.Get(), nullptr, 0);
        auto* buffer = hudConstants.Get();
        context->PSSetConstantBuffers(0, 1, &buffer);
        context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->IASetInputLayout(inputLayout.Get());
        auto* vertexBuffer = vertices.Get();
        constexpr UINT stride = sizeof(Float4), offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void RegisterNativeRole()
    {
        auto* native = hudShader.Get();
        CreatedShaderIdentity{.nativeSceneComposite = true}.Register(S_OK, &native);
        FO4HDR_WARP_CHECK(IsNativeSceneCompositeShader(native));
    }

    void Seed(Texture& target)
    {
        device.context->ClearRenderTargetView(target.rtv.Get(), stale);
    }

    void CheckPixels(Texture& target, float rejected, bool drawn)
    {
        device.context->CopyResource(target.staging.Get(), target.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read initialized scene target");
        std::array<std::array<std::uint16_t, 4>, kWidth> pixels{};
        std::memcpy(pixels.data(), mapped.pData, sizeof(pixels));
        device.context->Unmap(target.staging.Get(), 0);
        FO4HDR_WARP_CHECK(Binary16ToFloat(pixels[1][0]) == rejected);
        if (drawn)
            for (const UINT index : {0U, 2U, 3U, 4U, 5U, 6U, 7U})
                FO4HDR_WARP_CHECK(Binary16ToFloat(pixels[index][0]) == 0.25F);
    }

    void ObserveOnly(bool triangle = true, ID3D11DeviceContext* context = nullptr)
    {
        UINT calls{};
        ExecuteObservedDraw(*record, context ? context : device.context.Get(),
            {[](void* value) { ++*static_cast<UINT*>(value); }, &calls, triangle});
        FO4HDR_WARP_CHECK(calls == 1);
    }

    void CheckInitialization()
    {
        Seed(scene);
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, stale[0], true);
        RegisterNativeRole();
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, 0, true);
        CheckState();
        Seed(scene);
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, stale[0], true);
        CompletePresentFrame(false, false);
        FO4HDR_WARP_CHECK(std::ranges::all_of(g_scene.initializedTargets,
            [](const auto& entry) { return !entry; }));
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, 0, true);
        Seed(scene);
        ResetSceneResources();
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, 0, true);
        CheckGenerationAndTargets();
    }

    void CheckGenerationAndTargets()
    {
        Seed(scene);
        const auto generation = g_evidence.resourceGeneration.fetch_add(1) + 1;
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, 0, true);
        FO4HDR_WARP_CHECK(g_scene.initializationGeneration == generation);
        auto other = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Seed(other);
        auto* target = other.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, depthView.Get());
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(other, 0, true);
        Seed(scene);
        Bind();
        device.context->DrawIndexed(3, 0, 0);
        CheckPixels(scene, stale[0], true);
        FO4HDR_WARP_CHECK(std::ranges::count_if(g_scene.initializedTargets,
            [](const auto& entry) { return bool(entry); }) == 2);
    }

    void CheckState()
    {
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11DepthStencilView> view;
        UINT reference{}, mask{};
        FLOAT factors[4]{};
        device.context->OMGetDepthStencilState(&depth, &reference);
        device.context->OMGetBlendState(&blend, factors, &mask);
        device.context->OMGetRenderTargets(1, &target, &view);
        device.context->PSGetShader(&pixel, nullptr, nullptr);
        FO4HDR_WARP_CHECK(depth.Get() == depthState.Get());
        FO4HDR_WARP_CHECK(blend.Get() == blendState.Get());
        FO4HDR_WARP_CHECK(pixel.Get() == hudShader.Get());
        FO4HDR_WARP_CHECK(target.Get() == scene.rtv.Get() && view.Get() == depthView.Get());
    }

    void CheckRejections()
    {
        ResetSceneTargetInitialization();
        Seed(scene);
        ObserveOnly(false);
        CheckPixels(scene, stale[0], false);
        g_configuration.enabled = false;
        ObserveOnly();
        g_configuration.enabled = true;
        CheckPixels(scene, stale[0], false);
        record->nextPsSetShader(
            device.context.Get(), g_presentation.presentShader.Get(), nullptr, 0);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        record->nextPsSetShader(device.context.Get(), hudShader.Get(), nullptr, 0);
        DrawObservation staleObservation = *record;
        UINT calls{};
        ExecuteObservedDraw(staleObservation, device.context.Get(),
            {[](void* value) { ++*static_cast<UINT*>(value); }, &calls, true});
        FO4HDR_WARP_CHECK(calls == 1);
        CheckPixels(scene, stale[0], false);
        g_observedPresentThread = 0;
        ObserveOnly();
        g_observedPresentThread = GetCurrentThreadId();
        CheckPixels(scene, stale[0], false);
        auto foreign = CreateNativeContextDevice(device);
        ObserveOnly(true, foreign.context.Get());
        CheckPixels(scene, stale[0], false);
        CheckPredicationRejection();
        CheckQueryRejection();
        CheckCoverageRejection();
        CheckPublicTargetRejection();
        CheckTargetRejections();
        Bind();
        ObserveOnly();
        CheckPixels(scene, 0, false);
    }

    void CheckQueryRejection()
    {
        record->computeQueries.unknown = true;
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        record->computeQueries = {};
        record->computeQueries.Begin(this);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        record->computeQueries.End(this);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
    }

    void CheckCoverageRejection()
    {
        auto* context = device.context.Get();
        context->OMSetBlendState(blendState.Get(), nullptr, 0);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        D3D11_BLEND_DESC blend{};
        blendState->GetDesc(&blend);
        blend.AlphaToCoverageEnable = TRUE;
        ComPtr<ID3D11BlendState> alphaCoverage;
        CheckHresult(device.device->CreateBlendState(&blend, &alphaCoverage),
            "Create alpha coverage scene state");
        context->OMSetBlendState(alphaCoverage.Get(), nullptr, ~0U);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        Bind();
        const D3D11_VIEWPORT collapsed{0, 0, kWidth, kHeight, 1, 1};
        context->RSSetViewports(1, &collapsed);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        Bind();
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        Bind();
        D3D11_RASTERIZER_DESC rasterizer{};
        g_presentation.presentRasterizer->GetDesc(&rasterizer);
        rasterizer.FillMode = D3D11_FILL_WIREFRAME;
        ComPtr<ID3D11RasterizerState> wireframe;
        CheckHresult(device.device->CreateRasterizerState(&rasterizer, &wireframe),
            "Create wireframe scene state");
        context->RSSetState(wireframe.Get());
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        Bind();
    }

    void CheckPredicationRejection()
    {
        ComPtr<ID3D11Predicate> predicate;
        const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        CheckHresult(device.device->CreatePredicate(&description, &predicate),
            "Create scene predicate");
        device.context->Begin(predicate.Get());
        device.context->End(predicate.Get());
        device.context->SetPredication(predicate.Get(), FALSE);
        ObserveOnly();
        ComPtr<ID3D11Predicate> actual;
        BOOL value{};
        device.context->GetPredication(&actual, &value);
        FO4HDR_WARP_CHECK(actual.Get() == predicate.Get() && value == FALSE);
        device.context->SetPredication(nullptr, FALSE);
        CheckPixels(scene, stale[0], false);
    }

    void CheckPublicTargetRejection()
    {
        Seed(proxy);
        auto* target = proxy.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, depthView.Get());
        ObserveOnly();
        CheckPixels(proxy, stale[0], false);
    }

    void CheckTargetRejections()
    {
        auto* target = scene.rtv.Get();
        const std::array targets{target, proxy.rtv.Get()};
        device.context->OMSetRenderTargets(2, targets.data(), depthView.Get());
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        device.context->OMSetRenderTargets(1, &target, nullptr);
        ObserveOnly();
        CheckPixels(scene, stale[0], false);
        device.context->OMSetRenderTargets(1, &target, depthView.Get());
        const auto width = g_presentation.width;
        g_presentation.width = width / 2;
        ObserveOnly();
        g_presentation.width = width;
        CheckPixels(scene, stale[0], false);
    }
};

void CheckNativeSceneShaderIdentity()
{
    std::array<std::uint32_t, 5> header{0x43425844, 0xf97c127c, 0xd49c4526, 0x1a87d341, 0x2a3edc35};
    CreatedShaderIdentity identity;
    identity.Observe(header.data(), sizeof(header), false);
    FO4HDR_WARP_CHECK(identity.nativeSceneComposite == fo4hdr::IsOriginalRuntime());
    FO4HDR_WARP_CHECK(identity.descriptor == nullptr);
    CreatedShaderIdentity truncated;
    truncated.Observe(header.data(), sizeof(header) - 1, false);
    FO4HDR_WARP_CHECK(!truncated.nativeSceneComposite);
    ++header[1];
    CreatedShaderIdentity unknown;
    unknown.Observe(header.data(), sizeof(header), false);
    FO4HDR_WARP_CHECK(!unknown.nativeSceneComposite);
}

} // namespace

void VerifyNativeSceneInitialization(Device& device)
{
    CheckNativeSceneShaderIdentity();
    NativeSceneInitializationFixture fixture(device);
    fixture.CheckInitialization();
    fixture.CheckRejections();
}

} // namespace fo4hdr::tests::gpu

