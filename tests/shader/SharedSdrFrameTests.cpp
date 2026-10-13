// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "FrameTrace.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include <bit>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace fo4hdr::tests::gpu {
namespace {

std::string g_startupOutputTrace;

void CaptureStartupOutputTrace(std::string_view message)
{
    g_startupOutputTrace.append(message);
    g_startupOutputTrace.push_back('\n');
}

struct SdrDispatchScope
{
    ContextRecord& record;
    fo4hdr::Dispatch dispatch{record.nextDispatch};
    DispatchIndirect indirect{record.nextDispatchIndirect};

    explicit SdrDispatchScope(ContextRecord& value) : record(value)
    {
        record.nextDispatch = [](ID3D11DeviceContext*, UINT, UINT, UINT) {};
        record.nextDispatchIndirect = [](ID3D11DeviceContext*, ID3D11Buffer*, UINT) {};
    }

    ~SdrDispatchScope()
    {
        record.nextDispatch = dispatch;
        record.nextDispatchIndirect = indirect;
    }
};

enum class SdrUavWrite { dispatch, indirect, clearUint, clearFloat };
enum class SdrForeignWrite { copy, clear, clearUav };
enum class SdrForeignWritePhase { beforeDraw, afterLut, afterUi };
enum class SdrMenuPattern { lut, videoTiles, uiOnly };
enum class SdrLifecycleContent { video, retainedUi, publicCopy };
enum class SdrUavFrame { qualified, cleared, foreignCleared, unarmedCleared };
enum class SdrPresent { hold, advance };

struct SharedSdrContextFixture : SharedPresentationContextFixture
{
    Texture lut, history, nativeUi;
    ComPtr<ID3D11PixelShader> temporalShader, lateUiShader;
    ComPtr<ID3D11Buffer> lateUiConstants, dispatchArguments;

    explicit SharedSdrContextFixture(Device& value, UINT lutBindings =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) : SharedPresentationContextFixture(value)
    {
        lut = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, lutBindings);
        history = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        nativeUi = CreateTexture(*device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE);
        temporalShader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs);
        lateUiShader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
        CreatedShaderIdentity identity;
        identity.lateGameUiComposite = true;
        auto* shader = lateUiShader.Get();
        identity.Register(S_OK, &shader);
        FO4HDR_WARP_CHECK(IsLateGameUiShader(shader));
        HudConstants constants;
        constants.color = {1, 1, 1, 1};
        constants.rectangle = {0, 0, 1, 1};
        lateUiConstants = CreateConstantBuffer(*device.device.Get(), constants);
        FO4HDR_WARP_CHECK(EnsureSharedUiResources(device.device.Get()));
        FO4HDR_WARP_CHECK(record->shadow[kContextDispatch] == reinterpret_cast<void*>(&HookDispatch));
        FO4HDR_WARP_CHECK(record->shadow[kContextDispatchIndirect] ==
            reinterpret_cast<void*>(&HookDispatchIndirect));
        FO4HDR_WARP_CHECK(record->shadow[kContextClearUnorderedAccessViewUint] ==
            reinterpret_cast<void*>(&HookClearUnorderedAccessViewUint));
        FO4HDR_WARP_CHECK(record->shadow[kContextClearUnorderedAccessViewFloat] ==
            reinterpret_cast<void*>(&HookClearUnorderedAccessViewFloat));
        FO4HDR_WARP_CHECK(reinterpret_cast<void*>(record->nextDispatch) ==
            record->originalVtable[kContextDispatch]);
        FO4HDR_WARP_CHECK(reinterpret_cast<void*>(record->nextDispatchIndirect) ==
            record->originalVtable[kContextDispatchIndirect]);
        FO4HDR_WARP_CHECK(reinterpret_cast<void*>(record->nextClearUnorderedAccessViewUint) ==
            record->originalVtable[kContextClearUnorderedAccessViewUint]);
        FO4HDR_WARP_CHECK(reinterpret_cast<void*>(record->nextClearUnorderedAccessViewFloat) ==
            record->originalVtable[kContextClearUnorderedAccessViewFloat]);
    }

    ~SharedSdrContextFixture()
    {
        ResetShaderRegistry();
    }

    void BeginSdr(float videoWhite = 203)
    {
        ++g_frame;
        ResetSharedUiContent();
        g_scene = {};
        g_gameplayReadyApplied = false;
        PublishGraphicsLifecycle(true, false);
        PublishGraphicsPresentationBlocked(true);
        g_referenceHealthy = true;
        g_activeSettings.uiPaperWhiteNits = 203;
        g_activeSettings.videoWhiteNits = videoWhite;
        g_presentation.colorSpaceState = 1;
        g_evidence.hdrColorSpaceActive = true;
        FO4HDR_WARP_CHECK(ArmSharedSdrFrame(device.context.Get()));
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady() && SharedSdrPaperWhite() == 203);
        FO4HDR_WARP_CHECK(g_presentation.presentationProxyDomain == LinearColorDomain::vanillaSdrCodes);
    }

    void DrawOriginal(RenoDxShaderPass pass, Texture& target,
        ID3D11ShaderResourceView* input = nullptr, bool partial = false)
    {
        ID3D11ShaderResourceView* clear{};
        device.context->PSSetShaderResources(0, 1, &clear);
        BindHud();
        const bool temporal = pass == RenoDxShaderPass::taa || pass == RenoDxShaderPass::taa2;
        std::array<ID3D11RenderTargetView*, 2> targets{target.rtv.Get(),
            temporal ? proxy.rtv.Get() : nullptr};
        device.context->OMSetRenderTargets(temporal ? 2 : 1, targets.data(), nullptr);
        device.context->PSSetShaderResources(0, 1, &input);
        if (temporal) device.context->PSSetShader(temporalShader.Get(), nullptr, 0);
        ComPtr<ID3D11Buffer> partialConstants;
        if (partial)
        {
            HudConstants values;
            values.rectangle = {2, 0, kWidth, kHeight};
            partialConstants = CreateConstantBuffer(*device.device.Get(), values);
            auto* constants = partialConstants.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
        }
        record->currentPass = pass;
        record->currentPassKnown = true;
        FO4HDR_WARP_CHECK(!record->currentReplacementActive);
        device.context->Draw(3, 0);
    }

    void DrawLateUi(ID3D11ShaderResourceView* source = nullptr)
    {
        BindHud();
        device.context->PSSetShader(lateUiShader.Get(), nullptr, 0);
        auto* input = source ? source : nativeUi.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        auto* constants = lateUiConstants.Get();
        device.context->PSSetConstantBuffers(0, 1, &constants);
        device.context->Draw(3, 0);
    }

    void CheckSdrPresentation(double sceneNits, bool partial = false)
    {
        FO4HDR_WARP_CHECK(SharedSdrFrameReady());
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        g_activeSettings.uiPaperWhiteNits = 406;
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(203)));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(sceneNits)), partial ? 2 : 1);
        if (partial) CheckOutputCode(0, 1);
        CompletePresentFrame(true, false);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
    }

    void CheckLoadingAndVideo()
    {
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, lut);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
        FO4HDR_WARP_CHECK(SharedSdrFrameReady());
        DrawLateUi();
        CheckSdrPresentation(std::pow(0.5, 2.2) * 203);
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, lut);
        DrawOriginal(RenoDxShaderPass::taa2, history, lut.srv.Get(), true);
        DrawLateUi();
        CheckSdrPresentation(std::pow(0.5, 2.2) * 203, true);
        for (const float videoWhite : {100.0F, 203.0F, 300.0F})
        {
            BeginSdr(videoWhite);
            g_activeSettings.videoWhiteNits = 600;
            DrawOriginal(RenoDxShaderPass::video, proxy);
            FO4HDR_WARP_CHECK(SharedSdrFrameReady());
            DrawLateUi();
            CheckSdrPresentation(std::pow(0.5, 2.2) * videoWhite);
        }
    }

    void CheckProducerPresentation(double sceneNits, bool partial = false)
    {
        FO4HDR_WARP_CHECK(SharedSdrFrameReady());
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(sceneNits)), partial ? 2 : 0);
        if (partial) CheckOutputCode(0);
        CompletePresentFrame(true, false);
    }

    void CheckWithoutFinalUi()
    {
        for (const auto pass : {RenoDxShaderPass::taa, RenoDxShaderPass::taa2})
        {
            for (const bool partial : {false, true})
            {
                BeginSdr();
                DrawOriginal(RenoDxShaderPass::lut, lut);
                DrawOriginal(pass, history, lut.srv.Get(), partial);
                CheckProducerPresentation(std::pow(0.5, 2.2) * 203, partial);
            }
        }
        for (const float videoWhite : {100.0F, 203.0F, 300.0F})
        {
            BeginSdr(videoWhite);
            DrawOriginal(RenoDxShaderPass::video, proxy, nullptr, true);
            CheckProducerPresentation(std::pow(0.5, 2.2) * videoWhite, true);
        }
    }

    void DrawReplacement(Texture& target)
    {
        BindHud();
        auto* view = target.rtv.Get();
        device.context->OMSetRenderTargets(1, &view, nullptr);
        record->currentPass = RenoDxShaderPass::lut;
        record->currentPassKnown = true;
        record->currentReplacementActive = true;
        device.context->Draw(3, 0);
    }

    void CheckReplacementDestinations()
    {
        for (const auto phase : {SdrForeignWritePhase::beforeDraw,
                 SdrForeignWritePhase::afterLut, SdrForeignWritePhase::afterUi})
        {
            BeginSdr();
            if (phase == SdrForeignWritePhase::beforeDraw) DrawReplacement(history);
            DrawOriginal(RenoDxShaderPass::lut, lut);
            if (phase == SdrForeignWritePhase::afterLut) DrawReplacement(history);
            DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
            if (phase == SdrForeignWritePhase::afterUi) DrawReplacement(history);
            CheckProducerPresentation(std::pow(0.5, 2.2) * 203);
        }
        for (const bool originalLut : {false, true})
        {
            BeginSdr();
            if (originalLut) DrawOriginal(RenoDxShaderPass::lut, lut);
            DrawReplacement(lut);
            DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
            CheckReadableSdrPresentation(0.5, SdrPresent::advance);
        }
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::video, proxy);
        DrawReplacement(proxy);
        CheckReadableSdrPresentation(0.5, SdrPresent::advance);
    }

    void CheckStartupVideoWhite()
    {
        for (const bool lateReset : {false, true})
        {
            g_activeSettings.uiPaperWhiteNits = 100;
            FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
            FO4HDR_WARP_CHECK(SharedUiStartupPaperWhite() == 100);
            BeginSdr(300);
            FO4HDR_WARP_CHECK(SharedUiStartupPaperWhite() == 203);
            g_activeSettings.uiPaperWhiteNits = 406;
            FO4HDR_WARP_CHECK(ArmSharedSdrFrame(device.context.Get()));
            FO4HDR_WARP_CHECK(SharedUiStartupPaperWhite() == 203);
            DrawOriginal(RenoDxShaderPass::video, proxy);
            FO4HDR_WARP_CHECK(SharedSdrFrameReady());
            const auto emergency = SharedUiEmergencyPresentation();
            FO4HDR_WARP_CHECK(emergency && emergency->white == 203);
            if (lateReset) PublishGraphicsLifecycle(true, true);
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true) != lateReset);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(std::pow(0.5, 2.2) * 300)));
            FO4HDR_WARP_CHECK(g_presentation.colorSpaceState == 1 && g_evidence.hdrColorSpaceActive.load());
            FO4HDR_WARP_CHECK(g_evidence.productionActive.load() != lateReset);
            CompletePresentFrame(!lateReset, false);
        }
        device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
        device.context->CopyResource(proxy.texture.Get(), proxy.staging.Get());
        FO4HDR_WARP_CHECK(SharedUiStartupPaperWhite() == 203);
        FO4HDR_WARP_CHECK(!SharedUiStartupSdr() && SharedUiStartupAssumed());
        BeginSdr();
        FO4HDR_WARP_CHECK(SharedUiStartupPaperWhite() == 203);
    }

    void CheckStartupOutputTrace()
    {
        const bool requested = FrameTraceRequested();
        FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
        g_scene = {};
        g_presentation.colorSpaceState = 1;
        g_evidence.hdrColorSpaceActive = true;
        constexpr float sdr[4]{0.25F, 0.25F, 0.25F, 1};
        device.context->ClearRenderTargetView(proxy.rtv.Get(), sdr);
        FO4HDR_WARP_CHECK(InitializeFrameTrace(true) && ArmFrameTrace());
        FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(std::pow(0.25, 2.2) * 203)));
        FO4HDR_WARP_CHECK(g_presentation.colorSpaceState == 1 && g_evidence.hdrColorSpaceActive.load());
        TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {});
        CompletePresentFrame(false, false);
        g_presentation.constants = CreateConstantBuffer(*device.device.Get(), CompositionConstants{});
        FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
        FO4HDR_WARP_CHECK(SharedUiStartupSdr());
        CheckOutputCode(0);
        FO4HDR_WARP_CHECK(!g_evidence.hdrColorSpaceActive.load() && g_referenceHealthy.load());
        TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {});
        TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {});
        g_startupOutputTrace.clear();
        ReportFrameTrace(&CaptureStartupOutputTrace);
        const auto white = std::to_string(std::bit_cast<std::uint32_t>(203.0F));
        // Startup output still carries the incomplete-scene rejection (7).
        FO4HDR_WARP_CHECK(g_startupOutputTrace.find("args=1/7/16/" + white + "/") != std::string::npos);
        FO4HDR_WARP_CHECK(g_startupOutputTrace.find("args=2/7/16/") != std::string::npos);
        CompletePresentFrame(false, false);
        CancelFrameTrace();
        ReportFrameTrace(&CaptureStartupOutputTrace);
        (void)InitializeFrameTrace(requested);
    }

    void CheckSdrMissingProducer()
    {
        BeginSdr();
        DrawLateUi();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        DrawOriginal(RenoDxShaderPass::lut, lut);
        DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
        DrawLateUi();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
        DrawLateUi();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, lut);
        DrawOriginal(RenoDxShaderPass::taa, history, nativeUi.srv.Get());
        DrawLateUi();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::video, proxy);
        DrawLateUi(lut.srv.Get());
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
    }

    void CheckObservedQuery(D3D11_QUERY type)
    {
        const D3D11_QUERY_DESC description{type, 0};
        ComPtr<ID3D11Query> query;
        CheckHresult(device.device->CreateQuery(&description, &query), "Create observed SDR query");
        BeginSdr();
        device.context->Begin(query.Get());
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch() != IsComputeSensitiveQuery(type));
        constexpr float black[]{0, 0, 0, 1};
        device.context->ClearRenderTargetView(lut.rtv.Get(), black);
        device.context->CopyResource(nativeUi.staging.Get(), nativeUi.texture.Get());
        DrawOriginal(RenoDxShaderPass::lut, lut);
        DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
        DrawLateUi();
        device.context->End(query.Get());
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        CheckSdrPresentation(std::pow(0.5, 2.2) * 203);
        BeginSdr();
        device.context->Begin(query.Get());
        DrawOriginal(RenoDxShaderPass::video, proxy);
        DrawLateUi();
        device.context->End(query.Get());
        CheckSdrPresentation(std::pow(0.5, 2.2) * 203);
    }

    void CheckQueryInjectionGuards()
    {
        const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION, 0};
        ComPtr<ID3D11Query> query;
        CheckHresult(device.device->CreateQuery(&description, &query), "Create guarded SDR query");
        BeginSdr();
        device.context->CopyResource(proxy.texture.Get(), history.texture.Get());
        device.context->Begin(query.Get());
        CompletePresentFrame(false, false);
        FO4HDR_WARP_CHECK(!ArmSharedSdrFrame(device.context.Get()));
        FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
        device.context->End(query.Get());
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        FO4HDR_WARP_CHECK(ArmSharedSdrFrame(device.context.Get()) && SharedUiStartupPaperWhite() == 203);
        BeginSdr(100);
        device.context->Begin(query.Get());
        DrawOriginal(RenoDxShaderPass::video, proxy);
        DrawLateUi();
        device.context->End(query.Get());
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    }

    Texture CreateLutViews(UINT mips, UINT slices)
    {
        D3D11_TEXTURE2D_DESC description{};
        lut.texture->GetDesc(&description);
        description.MipLevels = mips;
        description.ArraySize = slices;
        Texture result;
        CheckHresult(device.device->CreateTexture2D(&description, nullptr, &result.texture),
            "Create unsupported LUT subresources");
        D3D11_RENDER_TARGET_VIEW_DESC target{};
        target.Format = description.Format;
        target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC source{};
        source.Format = description.Format;
        source.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        source.Texture2D.MipLevels = mips;
        if (slices > 1)
        {
            target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            target.Texture2DArray.FirstArraySlice = 1;
            target.Texture2DArray.ArraySize = 1;
            source.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            source.Texture2DArray.MipLevels = mips;
            source.Texture2DArray.FirstArraySlice = 1;
            source.Texture2DArray.ArraySize = 1;
        }
        CheckHresult(device.device->CreateRenderTargetView(result.texture.Get(), &target, &result.rtv),
            "Create unsupported LUT target view");
        CheckHresult(device.device->CreateShaderResourceView(result.texture.Get(), &source, &result.srv),
            "Create unsupported LUT source view");
        return result;
    }

    void CheckLutSubresources()
    {
        for (const auto dimensions : {std::array{2U, 1U}, std::array{1U, 2U}})
        {
            auto unsupported = CreateLutViews(dimensions[0], dimensions[1]);
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::lut, unsupported);
            DrawOriginal(RenoDxShaderPass::taa, history, unsupported.srv.Get());
            DrawLateUi();
            FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
            FO4HDR_WARP_CHECK(g_referenceHealthy.load());
        }
    }

    void DrawWithOutputUav(Texture& target, SdrUavFrame frame)
    {
        BindHud();
        const UINT slot = device.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 8U : 0U;
        auto* uav = target.uav.Get();
        device.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
            slot, 1, &uav, nullptr);
        if (frame == SdrUavFrame::foreignCleared) std::thread([&] { device.context->Draw(3, 0); }).join();
        else device.context->Draw(3, 0);
        uav = nullptr;
        device.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
            slot, 1, &uav, nullptr);
    }

    void CheckOutputUav()
    {
        auto unrelated = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_UNORDERED_ACCESS);
        lut = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        proxy = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        RetireSharedUiPublicImage();
        g_presentation.presentationProxy = proxy.texture;
        g_presentation.presentationProxyRtv = proxy.rtv;
        g_presentation.presentationProxySrv = proxy.srv;
        constexpr float black[]{0, 0, 0, 1};
        for (const auto frame : {SdrUavFrame::qualified, SdrUavFrame::cleared,
                 SdrUavFrame::foreignCleared, SdrUavFrame::unarmedCleared})
        for (auto* target : {&unrelated, &lut, &proxy})
        {
            BeginSdr();
            if (frame == SdrUavFrame::unarmedCleared) ResetSharedSdrFrame();
            if (frame != SdrUavFrame::qualified)
                device.context->ClearRenderTargetView(proxy.rtv.Get(), black);
            DrawOriginal(RenoDxShaderPass::lut, lut);
            DrawWithOutputUav(*target, frame);
            DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
            if (target == &unrelated && frame == SdrUavFrame::qualified)
                CheckProducerPresentation(std::pow(0.5, 2.2) * 203);
            else
                CheckReadableSdrPresentation(0.5, SdrPresent::advance);
        }
    }

    void CheckVideoScaleWithStreamOutput()
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = 16;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        ComPtr<ID3D11Buffer> stream;
        CheckHresult(device.device->CreateBuffer(&description, nullptr, &stream),
            "Create occupied video scaling stream output");
        BeginSdr(100);
        auto* buffer = stream.Get();
        constexpr UINT offset = 0;
        device.context->SOSetTargets(1, &buffer, &offset);
        DrawOriginal(RenoDxShaderPass::video, proxy);
        FO4HDR_WARP_CHECK(SharedSdrFrameReady() && g_referenceHealthy.load());
        device.context->SOSetTargets(0, nullptr, nullptr);
        CheckProducerPresentation(std::pow(0.5, 2.2) * 100);
        FO4HDR_WARP_CHECK(!ScaleSharedSdrVideo(nullptr, 100, 203));
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    }

    void CompleteVideo()
    {
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::video, proxy);
        DrawLateUi();
        FO4HDR_WARP_CHECK(SharedSdrFrameReady());
    }

    void CheckCommandListMutation()
    {
        ComPtr<ID3D11DeviceContext> deferred;
        ComPtr<ID3D11CommandList> commands;
        CheckHresult(device.device->CreateDeferredContext(0, &deferred),
            "Create deferred original SDR mutation context");
        constexpr float gray[]{0.25F, 0.25F, 0.25F, 1};
        deferred->ClearRenderTargetView(proxy.rtv.Get(), gray);
        CheckHresult(deferred->FinishCommandList(FALSE, &commands),
            "Record deferred original SDR mutation");
        for (const BOOL restoreState : {FALSE, TRUE})
        {
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::video, proxy);
            FO4HDR_WARP_CHECK(SharedSdrFrameReady() && SharedUiStartupSdr());
            device.context->ExecuteCommandList(commands.Get(), restoreState);
            FO4HDR_WARP_CHECK(!SharedSdrFrameReady() && !SharedUiStartupSdr());
            FO4HDR_WARP_CHECK(SharedUiStartupAssumed() && SharedUiStartupPaperWhite() == 203);
            CheckReadableSdrPresentation(0.25, SdrPresent::advance);
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::video, proxy);
            CheckProducerPresentation(std::pow(0.5, 2.2) * 203);
        }
    }

    void CheckSdrMutationAndReset()
    {
        CompleteVideo();
        bool foreignReady{true};
        std::thread foreign([&] { foreignReady = SharedSdrFrameReady(); });
        foreign.join();
        FO4HDR_WARP_CHECK(!foreignReady && SharedSdrFrameReady());
        CompleteVideo();
        device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
        device.context->CopyResource(proxy.texture.Get(), proxy.staging.Get());
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        ResetSharedSdrFrame();
        FO4HDR_WARP_CHECK(!ArmSharedSdrFrame(device.context.Get()));
        CompleteVideo();
        constexpr float white[]{1, 1, 1, 1};
        device.context->ClearRenderTargetView(proxy.rtv.Get(), white);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        CompleteVideo();
        BindHud();
        auto* target = proxy.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        record->currentReplacementActive = true;
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        CompleteVideo();
        DrawOriginal(RenoDxShaderPass::video, proxy);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        CompleteVideo();
        const auto generation = g_evidence.resourceGeneration.fetch_add(1);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        g_evidence.resourceGeneration = generation;
        const auto contextGeneration = record->generation++;
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        record->generation = contextGeneration;
        ResetSharedSdrFrame();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        FO4HDR_WARP_CHECK(!ArmSharedSdrFrame(device.context.Get()));
        CompleteVideo();
        PublishGraphicsLifecycle(true, true);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
    }

    void CheckIdempotentArm()
    {
        CompleteVideo();
        g_activeSettings.uiPaperWhiteNits = 406;
        FO4HDR_WARP_CHECK(ArmSharedSdrFrame(device.context.Get()));
        FO4HDR_WARP_CHECK(SharedSdrFrameReady() && SharedSdrPaperWhite() == 203);
        constexpr float white[]{1, 1, 1, 1};
        device.context->ClearRenderTargetView(proxy.rtv.Get(), white);
        FO4HDR_WARP_CHECK(!ArmSharedSdrFrame(device.context.Get()));
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
    }

    void CheckRenderThreadMigration(bool afterWriter)
    {
        BeginSdr();
        if (afterWriter) DrawOriginal(RenoDxShaderPass::lut, lut);
        std::exception_ptr failure;
        std::thread renderThread([&] {
            try
            {
                if (!afterWriter) DrawOriginal(RenoDxShaderPass::lut, lut);
                CompleteLoading();
                FO4HDR_WARP_CHECK(SharedSdrFrameReady() != afterWriter);
                FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true) != afterWriter);
                CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(203)));
                CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(std::pow(0.5, 2.2) * 203)), 1);
                CompletePresentFrame(!afterWriter, false);
            }
            catch (...) { failure = std::current_exception(); }
        });
        renderThread.join();
        if (failure) std::rethrow_exception(failure);
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
    }

    void WriteForeign(SdrForeignWrite operation, Texture& target)
    {
        std::thread worker([&] {
            constexpr float black[]{0, 0, 0, 1};
            if (operation == SdrForeignWrite::copy)
            {
                auto* source = target.texture.Get() == lut.texture.Get() ?
                    history.texture.Get() : lut.texture.Get();
                device.context->CopySubresourceRegion(target.texture.Get(), 0, 0, 0, 0, source, 0, nullptr);
            }
            else if (operation == SdrForeignWrite::clear)
                device.context->ClearRenderTargetView(target.rtv.Get(), black);
            else
                device.context->ClearUnorderedAccessViewFloat(target.uav.Get(), black);
        });
        worker.join();
    }

    void CheckReadableSdrPresentation(double sceneCode, SdrPresent completion)
    {
        const auto productionFrames = g_evidence.productionFrameCount.load();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady() && !SharedUiFrameReady());
        FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(std::pow(sceneCode, 2.2) * 203)), 1);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load() && !g_evidence.productionActive.load());
        FO4HDR_WARP_CHECK(g_evidence.hdrColorSpaceActive.load());
        FO4HDR_WARP_CHECK(g_evidence.productionFrameCount.load() == productionFrames);
        if (completion == SdrPresent::advance) CompletePresentFrame(false, false);
    }

    void CheckForeignWrite(SdrForeignWrite operation, SdrForeignWritePhase phase, Texture& target)
    {
        BeginSdr();
        if (phase != SdrForeignWritePhase::beforeDraw) DrawOriginal(RenoDxShaderPass::lut, lut);
        if (phase == SdrForeignWritePhase::afterUi) CompleteLoading();
        WriteForeign(operation, target);
        if (phase == SdrForeignWritePhase::beforeDraw) DrawOriginal(RenoDxShaderPass::lut, lut);
        if (phase != SdrForeignWritePhase::afterUi) CompleteLoading();
        if (target.texture.Get() == proxy.texture.Get())
        {
            if (operation != SdrForeignWrite::copy)
                CheckReadableSdrPresentation(phase == SdrForeignWritePhase::afterUi ? 0 : 0.5,
                    SdrPresent::advance);
            else CheckReadableSdrPresentation(0.5, SdrPresent::advance);
        }
        else
            CheckSdrPresentation(std::pow(0.5, 2.2) * 203);
    }

    void CheckForeignLutWrites(SdrForeignWrite operation)
    {
        BeginSdr();
        constexpr float gray[]{0.25F, 0.25F, 0.25F, 1};
        device.context->ClearRenderTargetView(history.rtv.Get(), gray);
        DrawOriginal(RenoDxShaderPass::lut, lut);
        WriteForeign(operation, lut);
        CompleteLoading();
        CheckReadableSdrPresentation(operation == SdrForeignWrite::copy ? 0.25 : 0, SdrPresent::advance);

        auto replacement = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                (operation == SdrForeignWrite::clearUav ? D3D11_BIND_UNORDERED_ACCESS : 0));
        for (const bool replace : {false, true})
        {
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::lut, lut);
            WriteForeign(operation, lut);
            auto& fresh = replace ? replacement : lut;
            DrawOriginal(RenoDxShaderPass::lut, fresh);
            if (replace) WriteForeign(operation, lut);
            DrawOriginal(RenoDxShaderPass::taa, history, fresh.srv.Get());
            DrawLateUi();
            CheckSdrPresentation(std::pow(0.5, 2.2) * 203);
        }

        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, lut);
        CompleteLoading();
        WriteForeign(operation, lut);
        CheckSdrPresentation(std::pow(0.5, 2.2) * 203);

        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, lut);
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, replacement);
        WriteForeign(operation, lut);
        DrawOriginal(RenoDxShaderPass::taa, history, replacement.srv.Get());
        DrawLateUi();
        CheckSdrPresentation(std::pow(0.5, 2.2) * 203);

        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, replacement);
        device.context->ClearRenderTargetView(lut.rtv.Get(), gray);
        WriteForeign(operation, replacement);
        DrawOriginal(RenoDxShaderPass::taa, history, replacement.srv.Get());
        DrawLateUi();
        CheckReadableSdrPresentation(operation == SdrForeignWrite::copy ? 0.25 : 0, SdrPresent::advance);
    }

    void CheckForeignWrites(SdrForeignWrite operation, Texture& unrelated)
    {
        for (const auto phase : {SdrForeignWritePhase::beforeDraw,
                 SdrForeignWritePhase::afterLut, SdrForeignWritePhase::afterUi})
        {
            CheckForeignWrite(operation, phase, unrelated);
            CheckForeignWrite(operation, phase, proxy);
        }
        CheckForeignLutWrites(operation);
    }

    void CheckWriterGenerationReuse()
    {
        BeginSdr();
        DrawOriginal(RenoDxShaderPass::lut, lut);
        const auto frame = g_frame;
        const auto generation = g_evidence.resourceGeneration.fetch_add(1);
        ResetSharedSdrFrame();
        FO4HDR_WARP_CHECK(g_frame == frame && ArmSharedSdrFrame(device.context.Get()));
        DrawOriginal(RenoDxShaderPass::lut, lut);
        CompleteLoading();
        FO4HDR_WARP_CHECK(SharedSdrFrameReady());
        g_evidence.resourceGeneration = generation;
    }

    void CheckGameplayReturnToMenu(bool loadingBlocked, SdrMenuPattern pattern)
    {
        PrepareScene();
        g_activeSettings.uiPaperWhiteNits = 406;
        PublishSharedUiScene(CaptureSharedUiWrite(true));
        DrawHud(false);
        PresentEncodedUi(0.5, 406);
        FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
        const auto gameplay = SharedUiPresentationForOutput().value_or(SharedUiPresentation{});
        FO4HDR_WARP_CHECK(gameplay.white == 406);
        FO4HDR_WARP_CHECK(gameplay.domain == LinearColorDomain::encodedSrgbBt709UiRelative);
        if (loadingBlocked)
        {
            PublishGraphicsPresentationBlocked(true);
            PublishGraphicsLifecycle(true, true);
            BindHud();
            FO4HDR_WARP_CHECK(!g_gameplayReadyApplied.load());
        }
        const auto generation = g_evidence.resourceGeneration.load();
        g_activeSettings.uiPaperWhiteNits = 203;
        g_activeSettings.videoWhiteNits = 100;
        PublishGraphicsLifecycle(false, true);
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        BindHud();
        FO4HDR_WARP_CHECK(ArmSharedSdrFrame(device.context.Get()));
        FO4HDR_WARP_CHECK(!g_gameplayReadyApplied.load() && !g_evidence.gameplayReady.load());
        FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() > generation);
        FO4HDR_WARP_CHECK(g_evidence.gameResetEpoch.load() == ReadGraphicsLifecycle().resetEpoch);
        FO4HDR_WARP_CHECK(!SharedUiFrameEncoded() && !SharedUiPresentationForOutput());
        FO4HDR_WARP_CHECK(!g_scene.productionOperatorApplied && !g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(SharedSdrPaperWhite() == 203 &&
            g_presentation.presentationProxyDomain == LinearColorDomain::vanillaSdrCodes);
        if (pattern == SdrMenuPattern::lut)
        {
            DrawOriginal(RenoDxShaderPass::lut, lut);
            CompleteLoading();
            CheckSdrPresentation(std::pow(0.5, 2.2) * 203);
        }
        else CheckMenuFallback(pattern);
        g_activeSettings.uiPaperWhiteNits = 203;
        auto savedCpuLut = std::move(g_activeCpuLut);
        g_activeCpuLut = std::make_unique<OperatorLutBuildResult>();
        g_activeCpuLut->data = BuildOperatorLut(g_activeSettings.productionAlgorithm,
            g_activeSettings.displayPeakNits, ActiveToneMapperParameters(
                g_activeSettings.productionAlgorithm, g_activeSettings.toneMapperParameters));
        FO4HDR_WARP_CHECK(BuildReplacementShaders(device.device.Get()));
        g_activeCpuLut = std::move(savedCpuLut);
        FO4HDR_WARP_CHECK(CurrentWideGamutReadiness().Complete());
        PublishGraphicsLifecycle(true, false);
        PublishGraphicsPresentationBlocked(false);
        BindHud();
        FO4HDR_WARP_CHECK(g_gameplayReadyApplied.load());
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
        PrepareScene();
        DrawHud(false);
        PresentEncodedUi(0.5);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load() && g_evidence.productionActive.load());
    }

    void CheckMenuFallback(SdrMenuPattern pattern)
    {
        const auto generation = g_evidence.resourceGeneration.load();
        for (unsigned frame = 0; frame < 2; ++frame)
        {
            {
                constexpr float black[]{0, 0, 0, 1};
                device.context->ClearRenderTargetView(proxy.rtv.Get(), black);
                device.context->ClearRenderTargetView(proxy.rtv.Get(), black);
                if (pattern == SdrMenuPattern::videoTiles)
                    for (unsigned tile = 0; tile < 40; ++tile)
                        DrawOriginal(RenoDxShaderPass::video, proxy, nullptr, true);
                DrawLateUi();
                BindHud();
                HudConstants values;
                values.color = {0.25F, 0.25F, 0.25F, 1};
                values.rectangle = {1, 0, 2, kHeight};
                const auto constants = CreateConstantBuffer(*device.device.Get(), values);
                auto* buffer = constants.Get();
                device.context->PSSetConstantBuffers(0, 1, &buffer);
                const ArmorUiScope scope({.owner = ArmorUiOwner::screenSpace, .threadId = GetCurrentThreadId()});
                FO4HDR_WARP_CHECK(scope.Entered());
                device.context->Draw(3, 0);
            }
            FO4HDR_WARP_CHECK(SharedUiStartupPaperWhite() == 203);
            g_activeSettings.uiPaperWhiteNits = 406;
            for (unsigned present = 0; present < 2; ++present)
            {
                CheckReadableSdrPresentation(0.25, SdrPresent::hold);
                CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(203)));
                CheckOutputCode(pattern == SdrMenuPattern::videoTiles ?
                    QuantizeR10Unorm(EncodeSt2084Pq(std::pow(0.5, 2.2) * 203)) : 0, 2);
                FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() == generation);
            }
            g_activeSettings.uiPaperWhiteNits = 203;
            CompletePresentFrame(false, false);
        }
    }

    void CheckLifecycleAfterProducer(SdrLifecycleContent content)
    {
        CompleteVideo();
        if (content == SdrLifecycleContent::retainedUi)
        {
            CompletePresentFrame(false, false);
            device.context->Draw(3, 0);
        }
        if (content == SdrLifecycleContent::publicCopy)
        {
            const auto clearedRevision = InvalidateSharedUiPresentation();
            device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
            device.context->CopyResource(proxy.texture.Get(), proxy.staging.Get());
            FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
            RefreshSharedUiStartupWhite(clearedRevision);
            FO4HDR_WARP_CHECK(!SharedUiStartupSdr() && !ArmSharedSdrFrame(device.context.Get()));
        }
        const auto generation = g_evidence.resourceGeneration.load();
        const auto productionFrames = g_evidence.productionFrameCount.load();
        const auto fallbackFrames = g_evidence.sdrFallbackFrameCount.load();
        PublishGraphicsLifecycle(true, true);
        BindHud();
        FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() == generation);
        if (content == SdrLifecycleContent::publicCopy)
        {
            CheckReadableSdrPresentation(0.5, SdrPresent::hold);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(203)));
            FO4HDR_WARP_CHECK(!SharedUiStartupSdr() && !SharedSdrFrameReady());
        }
        else
        {
            CheckReadableSdrPresentation(content == SdrLifecycleContent::retainedUi ? 0 : 0.5,
                SdrPresent::hold);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(203)));
        }
        FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() > generation);
        FO4HDR_WARP_CHECK(g_presentation.colorSpaceState == 1 && g_evidence.hdrColorSpaceActive.load());
        FO4HDR_WARP_CHECK(g_evidence.productionFrameCount.load() == productionFrames);
        FO4HDR_WARP_CHECK(g_evidence.sdrFallbackFrameCount.load() == fallbackFrames);
        CompletePresentFrame(false, false);
    }

    void WriteUav(SdrUavWrite operation, Texture& target)
    {
        device.context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* noSource{};
        device.context->PSSetShaderResources(0, 1, &noSource);
        const UINT slot = device.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
            D3D11_1_UAV_SLOT_COUNT - 1 : D3D11_PS_CS_UAV_REGISTER_COUNT - 1;
        ID3D11UnorderedAccessView* view{};
        device.context->CSSetUnorderedAccessViews(slot, 1, &view, nullptr);
        constexpr UINT integers[]{1, 2, 3, 4};
        constexpr FLOAT floats[]{0, 0, 0, 1};
        if (operation == SdrUavWrite::clearUint)
            device.context->ClearUnorderedAccessViewUint(target.uav.Get(), integers);
        else if (operation == SdrUavWrite::clearFloat)
            device.context->ClearUnorderedAccessViewFloat(target.uav.Get(), floats);
        else
        {
            view = target.uav.Get();
            device.context->CSSetUnorderedAccessViews(slot, 1, &view, nullptr);
            ComPtr<ID3D11UnorderedAccessView> bound;
            device.context->CSGetUnorderedAccessViews(slot, 1, &bound);
            FO4HDR_WARP_CHECK(bound.Get() == view);
            if (operation == SdrUavWrite::dispatch)
                device.context->Dispatch(1, 2, 3);
            else
                device.context->DispatchIndirect(dispatchArguments.Get(), 12);
            view = nullptr;
            device.context->CSSetUnorderedAccessViews(slot, 1, &view, nullptr);
        }
    }

    void CompleteLoading()
    {
        DrawOriginal(RenoDxShaderPass::taa, history, lut.srv.Get());
        DrawLateUi();
    }

    void CheckNativeUavWrites()
    {
        SdrDispatchScope forwarding(*record);
        D3D11_BUFFER_DESC arguments{};
        arguments.ByteWidth = 24;
        arguments.Usage = D3D11_USAGE_DEFAULT;
        arguments.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        CheckHresult(device.device->CreateBuffer(&arguments, nullptr, &dispatchArguments),
            "Create observed SDR indirect dispatch arguments");
        proxy = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        RetireSharedUiPublicImage();
        g_presentation.presentationProxy = proxy.texture;
        g_presentation.presentationProxyRtv = proxy.rtv;
        g_presentation.presentationProxySrv = proxy.srv;
        auto unrelated = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_UNORDERED_ACCESS);
        for (const auto operation : {SdrUavWrite::dispatch, SdrUavWrite::indirect,
                 SdrUavWrite::clearUint, SdrUavWrite::clearFloat})
        {
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::lut, lut);
            WriteUav(operation, lut);
            CompleteLoading();
            FO4HDR_WARP_CHECK(!SharedSdrFrameReady() && SharedUiStartupSdr());
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::lut, lut);
            WriteUav(operation, unrelated);
            CompleteLoading();
            FO4HDR_WARP_CHECK(SharedSdrFrameReady() && SharedUiStartupSdr());
            WriteUav(operation, unrelated);
            FO4HDR_WARP_CHECK(SharedSdrFrameReady());
            WriteUav(operation, proxy);
            FO4HDR_WARP_CHECK(!SharedSdrFrameReady() && !SharedUiStartupSdr());
            FO4HDR_WARP_CHECK(SharedUiStartupAssumed() && SharedUiStartupPaperWhite() == 203);
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::lut, lut);
            {
                ScopedPresentationContext owned(device.context.Get());
                FO4HDR_WARP_CHECK(owned.Ready());
                WriteUav(operation, lut);
            }
            CompleteLoading();
            FO4HDR_WARP_CHECK(SharedSdrFrameReady());
            {
                ScopedPresentationContext owned(device.context.Get());
                FO4HDR_WARP_CHECK(owned.Ready());
                WriteUav(operation, proxy);
            }
            FO4HDR_WARP_CHECK(SharedSdrFrameReady());
            WriteUav(operation, proxy);
            FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
            BeginSdr();
            DrawOriginal(RenoDxShaderPass::lut, lut);
            CompleteLoading();
            FO4HDR_WARP_CHECK(SharedSdrFrameReady() && g_referenceHealthy.load());
        }
    }
};

void VerifyExtendedSdrUavSlots(Device& source)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    CheckHresult(source.device.As(&dxgi), "Get extended SDR adapter device");
    CheckHresult(dxgi->GetAdapter(&adapter), "Get extended SDR adapter");
    Device native;
    constexpr D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_1;
    const auto result = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        0, &requested, 1, D3D11_SDK_VERSION, &native.device, nullptr, &native.context);
    if (result == E_INVALIDARG || result == DXGI_ERROR_UNSUPPORTED)
    {
        ++g_skips;
        std::cout << "Shared SDR UAV slot 63 skipped: feature level 11_1 unavailable\n";
        return;
    }
    CheckHresult(result, "Create extended SDR native context");
    SharedSdrContextFixture fixture(native,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    fixture.CheckNativeUavWrites();
}

} // namespace

void VerifySharedSdrFrame(Device& device)
{
    auto native = CreateNativeContextDevice(device);
    {
        SharedSdrContextFixture fixture(native);
        fixture.CheckStartupVideoWhite();
    }
    {
        SharedSdrContextFixture fixture(native);
        fixture.CheckStartupOutputTrace();
    }
    for (const bool loadingBlocked : {false, true})
    {
        SharedSdrContextFixture fixture(native);
        fixture.CheckGameplayReturnToMenu(loadingBlocked, SdrMenuPattern::lut);
    }
    for (const auto pattern : {SdrMenuPattern::videoTiles, SdrMenuPattern::uiOnly})
    {
        SharedSdrContextFixture fixture(native);
        fixture.CheckGameplayReturnToMenu(false, pattern);
    }
    {
        SharedSdrContextFixture fixture(native);
        fixture.CheckLoadingAndVideo();
        fixture.CheckWithoutFinalUi();
        fixture.CheckReplacementDestinations();
        fixture.CheckCommandListMutation();
        fixture.CheckObservedQuery(D3D11_QUERY_OCCLUSION);
        fixture.CheckObservedQuery(D3D11_QUERY_PIPELINE_STATISTICS);
        fixture.CheckQueryInjectionGuards();
        fixture.CheckSdrMissingProducer();
        fixture.CheckLutSubresources();
        fixture.CheckOutputUav();
        fixture.CheckSdrMutationAndReset();
        fixture.CheckIdempotentArm();
        fixture.CheckRenderThreadMigration(false);
        fixture.CheckRenderThreadMigration(true);
        fixture.CheckForeignWrites(SdrForeignWrite::copy, fixture.history);
        fixture.CheckForeignWrites(SdrForeignWrite::clear, fixture.history);
        fixture.CheckWriterGenerationReuse();
        fixture.CheckLifecycleAfterProducer(SdrLifecycleContent::video);
        fixture.CheckLifecycleAfterProducer(SdrLifecycleContent::retainedUi);
        fixture.CheckLifecycleAfterProducer(SdrLifecycleContent::publicCopy);
        fixture.CheckVideoScaleWithStreamOutput();
    }
    {
        SharedSdrContextFixture nativeFixture(native,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        nativeFixture.CheckLoadingAndVideo();
        nativeFixture.CheckLutSubresources();
        nativeFixture.CheckNativeUavWrites();
        auto unrelated = CreateTexture(*native.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_UNORDERED_ACCESS);
        nativeFixture.CheckForeignWrites(SdrForeignWrite::clearUav, unrelated);
    }
    VerifyExtendedSdrUavSlots(device);
}

} // namespace fo4hdr::tests::gpu
