// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "PresentationContextFixture.hpp"
#include "reference/ShaderReference.hpp"
#include "FrameTrace.hpp"
#include "FO4Isotope/config/PluginConfiguration.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace fo4hdr::tests::gpu {

PresentationContextFixture::PresentationContextFixture(Device& value) : device(value)
{
    RetireSharedUiPublicImage();
    g_presentation = {};
    g_uiTransport = {};
    g_scene = {};
    g_armor = {};
    g_armor.armorEncodedAttempted = true;
    g_rendererDevice.activeDevice = device.device.Get();
    g_rendererDevice.activeContext = device.context.Get();
    CheckHresult(device.context.As(&g_rendererDevice.activeContext1), "Get hooked presentation context1");
    g_configuration.enabled = true;
    g_configuration.productionRequested = true;
    g_configuration.measurePerformance = false;
    g_activeSettings.uiPaperWhiteNits = 203;
    g_activeSettings.displayPeakNits = 1000;
    g_referenceHealthy = true;
    g_productionHealthy = true;
    g_gameplayReadyApplied = true;
}

PresentationContextFixture::~PresentationContextFixture()
{
    if (record)
    {
        auto* context = device.context.Get();
        void* expected = record->shadow.data();
        std::atomic_ref<void*> slot(*reinterpret_cast<void**>(context));
        FO4HDR_WARP_CHECK(slot.compare_exchange_strong(expected, record->originalVtable));
        FO4HDR_WARP_CHECK(record->identity.Retire(context, record->generation));
        static_cast<DrawObservation&>(*record) = {};
    }
    device.context->ClearState();
    RetireSharedUiPublicImage();
    g_rendererDevice = savedDevice;
    g_presentation = savedPresentation;
    g_uiTransport = savedUi;
    g_scene = savedScene;
    g_armor = savedArmor;
    g_heartbeat = savedHeartbeat;
    g_configuration = savedConfiguration;
    g_activeSettings = savedSettings;
    g_referenceHealthy = savedHealthy;
    g_productionHealthy = savedProductionHealthy;
    g_gameplayReadyApplied = savedGameplay;
}

void PresentationContextFixture::PrepareResources()
{
    chain = CreatePresentationChain(device, window.handle, kWidth, kHeight);
    output = GetPresentationBuffer(device, *chain.Get());
    proxy = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    g_presentation.width = kWidth;
    g_presentation.height = kHeight;
    g_presentation.presentationBackBuffer = output.texture;
    g_presentation.presentationBackBufferRtv = output.rtv;
    RetireSharedUiPublicImage();
    g_presentation.presentationProxy = proxy.texture;
    g_presentation.presentationProxyRtv = proxy.rtv;
    g_presentation.presentationProxySrv = proxy.srv;
    g_presentation.presentationProxyDomain = LinearColorDomain::linearBt2020UiRelative;
    CheckHresult(chain.As(&g_presentation.swapChain3), "Get hooked presentation color control");
    g_presentation.colorSpaceSupported = true;
    g_presentation.sdrColorSpaceSupported = true;
    g_presentation.colorSpaceState = 1;
    const auto shaders = CreatePresentationShaders(*device.device.Get());
    g_presentation.presentVertexShader = shaders.vertex;
    g_presentation.presentShader = shaders.pixel;
    g_presentation.presentRasterizer = shaders.rasterizer;
    g_presentation.presentDepth = shaders.depth;
    g_presentation.presentBlend = shaders.blend;
    FO4HDR_WARP_CHECK(CreateTextureViews(device.device.Get(), kWidth, kHeight));
    FO4HDR_WARP_CHECK(RecreatePresentationConstants(device.device.Get()));
    g_scene.currentScene = proxy.srv;
    g_scene.currentSceneDomain = LinearColorDomain::linearBt2020UiRelative;
    constexpr float scene[4]{0.5F, 0.5F, 0.5F, 1.0F}, black[4]{0, 0, 0, 1};
    device.context->ClearRenderTargetView(proxy.rtv.Get(), scene);
    device.context->ClearRenderTargetView(output.rtv.Get(), black);
}

void PresentationContextFixture::InstallHooks()
{
    // RTTI reads just before the shadow table must see the pre-install slots;
    // the D3D11 context embeds its table, so read them before the vptr moves.
    const auto original = reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void***>(device.context.Get()));
    std::array<void*, kShadowHeaderSlots> header{};
    for (std::size_t index = 0; index < header.size(); ++index)
        header[index] = *reinterpret_cast<void* const*>(original - (header.size() - index) * sizeof(void*));
    FO4HDR_WARP_CHECK(InstallContextShadow(device.context.Get()));
    record = FindContext(device.context.Get());
    if (!record) throw std::runtime_error("Context hook test did not publish a record");
    FO4HDR_WARP_CHECK(*reinterpret_cast<void***>(device.context.Get()) == record->shadow.data());
    FO4HDR_WARP_CHECK(record->shadow.header == header);
    if (!g_configuration.productionRequested)
    {
        for (const auto slot : {kContextDispatch, kContextDispatchIndirect,
                 kContextClearUnorderedAccessViewUint, kContextClearUnorderedAccessViewFloat})
            FO4HDR_WARP_CHECK(record->shadow[slot] == record->originalVtable[slot]);
        if (!FrameTraceRequested())
        {
            for (const auto slot : {kContextCopyResource, kContextCopySubresourceRegion,
                     kContextClearRenderTargetView})
                FO4HDR_WARP_CHECK(record->shadow[slot] == record->originalVtable[slot]);
        }
        return;
    }
    auto* target = output.rtv.Get();
    device.context->OMSetRenderTargets(1, &target, nullptr);
    ComPtr<ID3D11RenderTargetView> redirected;
    device.context->OMGetRenderTargets(1, &redirected, nullptr);
    FO4HDR_WARP_CHECK(redirected.Get() == proxy.rtv.Get());
    record->nextPsSetShader(device.context.Get(), g_presentation.presentShader.Get(), nullptr, 0);
    device.context->VSSetShader(g_presentation.presentVertexShader.Get(), nullptr, 0);
    device.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    record->currentPass = RenoDxShaderPass::toneMapA;
    record->currentPassKnown = true;
    record->currentReplacementActive = true;
    record->expectedBoundShader = g_presentation.presentShader.Get();
    record->currentRequirement = PipelinePassRequirement::required;
}


SharedPresentationContextFixture::SharedPresentationContextFixture(Device& value) : PresentationContextFixture(value)
{
    ResetSharedUiResources();
    SetSharedUiPauseResourcesReady(false);
    static const auto shipped = LoadPluginConfiguration(std::filesystem::path{"config/FO4Isotope.ini"});
    FO4HDR_WARP_CHECK(shipped.valid);
    FO4HDR_WARP_CHECK(shipped.value.productionRequested && !shipped.value.developerMode);
    g_configuration = shipped.value;
    g_activeSettings = shipped.value;
    PrepareResources();
    FO4HDR_WARP_CHECK(EnsureSharedUiResources(device.device.Get()));
    InstallHooks();
    FO4HDR_WARP_CHECK(record->shadow[kContextCopyResource] == reinterpret_cast<void*>(&HookCopyResource));
    FO4HDR_WARP_CHECK(record->shadow[kContextClearRenderTargetView] ==
        reinterpret_cast<void*>(&HookClearRenderTargetView));
    g_presentation.applicationFacadeChain = chain.Get();
    g_presentation.r10Presentation = true;
    g_presentation.flipModel = true;
    g_presentation.bufferCount = 2;
    g_evidence.hdrColorSpaceActive = true;
    PublishGraphicsLifecycle(true, false);
    PublishGraphicsPresentationBlocked(false);
    g_lifecycle = {};
    FO4HDR_WARP_CHECK(g_lifecycle.BindDeviceAndSwapChain());
    (void)g_lifecycle.OnGameReset(savedGame.resetEpoch);
    FO4HDR_WARP_CHECK(g_lifecycle.PublishHdrResources());
    g_evidence.resourceGeneration = g_lifecycle.Snapshot().resourceGeneration;
    g_frame = 1;
    g_heartbeat.Reset(g_evidence.resourceGeneration.load());
    g_heartbeat.BeginFrame(g_evidence.resourceGeneration.load(), g_frame);
    g_observedPresentThread = GetCurrentThreadId();
    g_liveSettingsThreadCompatible = true;
    g_currentFrameQualifiedDrawThread = 0;
    g_currentFrameQualifiedDrawThreadConflict = false;
    g_replacements.operatorConstants = CreateConstantBuffer(*device.device.Get(), OperatorConstants{});
    PublishActiveDisplay(nullptr, -1);
    SetArmorUiCorrectionEnabled(true);
    SetArmorUiBoundaryCallback(SharedUiBoundary);
    CreateHudDraw();
}

SharedPresentationContextFixture::~SharedPresentationContextFixture()
{
    SetArmorUiBoundaryCallback(nullptr);
    SetArmorUiCorrectionEnabled(savedCorrection);
    SetSharedUiPauseResourcesReady(savedPauseResources);
    ResetSharedUiResources();
    PublishGraphicsLifecycle(savedGame.gameplayReady, false);
    PublishGraphicsPresentationBlocked(savedGame.presentationBlocked);
    PublishActiveDisplay(savedMonitor, savedHdrState);
    g_lifecycle = savedLifecycle;
    g_replacements = savedReplacements;
    g_frame = savedFrame;
}



void SharedPresentationContextFixture::CreateHudDraw()
{
    hudShader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
    hudConstants = CreateConstantBuffer(*device.device.Get(), HudConstants{});
}

void SharedPresentationContextFixture::BindHud()
{
    auto* target = proxy.rtv.Get();
    auto* constants = hudConstants.Get();
    device.context->OMSetRenderTargets(1, &target, nullptr);
    device.context->PSSetShader(hudShader.Get(), nullptr, 0);
    device.context->PSSetConstantBuffers(0, 1, &constants);
    device.context->VSSetShader(g_presentation.presentVertexShader.Get(), nullptr, 0);
    device.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device.context->RSSetState(g_presentation.presentRasterizer.Get());
    const D3D11_VIEWPORT viewport{0, 0, kWidth, kHeight, 0, 1};
    device.context->RSSetViewports(1, &viewport);
    device.context->OMSetDepthStencilState(g_presentation.presentDepth.Get(), 0);
    device.context->OMSetBlendState(g_presentation.presentBlend.Get(), nullptr, UINT(-1));
    FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget);
    FO4HDR_WARP_CHECK(!record->currentPassKnown && !record->currentReplacementActive);
}



void SharedPresentationContextFixture::PrepareScene()
{
    g_scene.productionOperatorApplied = false;
    g_scene.temporalPassObserved = false;
    g_scene.temporalSceneReady = false;
    constexpr float scene[4]{0.5F, 0.5F, 0.5F, 1};
    device.context->ClearRenderTargetView(proxy.rtv.Get(), scene);
    g_scene.currentScene = proxy.srv;
    g_scene.currentSceneDomain = LinearColorDomain::linearBt2020UiRelative;
    g_scene.productionOperatorApplied = true;
    g_scene.temporalPassObserved = true;
    g_scene.temporalSceneReady = true;
    g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
    const auto generation = g_evidence.resourceGeneration.load();
    g_presentation.colorDomainGeneration = generation;
    g_heartbeat.BeginFrame(generation, g_frame);
    g_heartbeat.Observe(RenoDxShaderPass::toneMapA);
    g_heartbeat.Observe(RenoDxShaderPass::lut);
    g_activeSettings.uiPaperWhiteNits = 203;
    // The fixture supplies accepted output evidence; physical display qualification is separate.
    g_presentation.colorSpaceState = 1;
    g_evidence.hdrColorSpaceActive = true;
    PublishSharedUiScene(CaptureSharedUiWrite(true));
}

void SharedPresentationContextFixture::DrawHud(bool write, ArmorUiOwner owner,
    std::uint32_t targetArgument)
{
    const bool encoded = SharedUiFrameEncoded();
    BindHud();
    const bool renderer = owner != ArmorUiOwner::screenSpace;
    const ArmorUiScope scope({.owner = owner, .threadId = GetCurrentThreadId(),
        .targetArgument = targetArgument, .postAa = renderer, .hasRoot = renderer});
    FO4HDR_WARP_CHECK(scope.Entered());
    FO4HDR_WARP_CHECK(!SharedUiFrameReady());
    if (!encoded && SharedUiFrameEncoded()) CheckEncodedSeed(proxy);
    if (write) device.context->Draw(3, 0);
}

void SharedPresentationContextFixture::CheckEncodedSeed(const Texture& source)
{
    device.context->CopyResource(source.staging.Get(), source.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(source.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read production shared UI seed");
    const auto values = *static_cast<const std::array<std::uint16_t, 4>*>(mapped.pData);
    device.context->Unmap(source.staging.Get(), 0);
    const auto encoded = g_armor.armorEncodedActive ? std::pow(0.5, 1.0 / 2.2) :
        1.055 * std::pow(0.5, 1.0 / 2.4) - 0.055;
    for (UINT channel = 0; channel < 3; ++channel)
        FO4HDR_WARP_CHECK(std::abs(Binary16ToFloat(values[channel]) - encoded) < 0.001);
    FO4HDR_WARP_CHECK(Binary16ToFloat(values[3]) == 1);
}

void SharedPresentationContextFixture::PresentEncodedUi(double linear, float white)
{
    FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
    FO4HDR_WARP_CHECK(SharedUiPaperWhite() == white);
    FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
    CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linear * white)));
    FO4HDR_WARP_CHECK(g_presentation.colorSpaceState == 1 && g_evidence.hdrColorSpaceActive.load());
    CompletePresentFrame(true, false);
    FO4HDR_WARP_CHECK(!SharedUiFrameReady());
}

void SharedPresentationContextFixture::CheckFrozenWhitePresentation()
{
    PrepareScene();
    DrawHud();
    g_activeSettings.uiPaperWhiteNits = 406;
    PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4));
}

std::array<std::array<std::uint16_t, 4>, kWidth> SharedPresentationContextFixture::ReadPublicPixels()
{
    // Diagnostic readback must not close an active armor composition scope.
    record->nextCopyResource(device.context.Get(), proxy.staging.Get(), proxy.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(proxy.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read current public image pixels");
    std::array<std::array<std::uint16_t, 4>, kWidth> pixels{};
    std::memcpy(pixels.data(), mapped.pData, sizeof(pixels));
    device.context->Unmap(proxy.staging.Get(), 0);
    return pixels;
}

std::uint32_t SharedPresentationContextFixture::ReadPresentationPixel(ID3D11Texture2D* texture)
{
    device.context->CopyResource(output.staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read presentation output");
    const auto pixel = *static_cast<const std::uint32_t*>(mapped.pData);
    device.context->Unmap(output.staging.Get(), 0);
    return pixel;
}

void SharedPresentationContextFixture::PresentReadable(double linear, float white)
{
    const auto generation = g_evidence.resourceGeneration.load();
    const auto productionFrames = g_evidence.productionFrameCount.load();
    FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
    CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linear * white)));
    FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() == generation);
    FO4HDR_WARP_CHECK(g_evidence.productionFrameCount.load() == productionFrames);
    FO4HDR_WARP_CHECK(!g_evidence.productionActive.load());
    FO4HDR_WARP_CHECK(g_evidence.hdrColorSpaceActive.load());
    CompletePresentFrame(false, false);
}

void SharedPresentationContextFixture::PresentBlack(bool hdr)
{
    const auto generation = g_evidence.resourceGeneration.load();
    const auto productionFrames = g_evidence.productionFrameCount.load();
    const auto fallbackFrames = g_evidence.sdrFallbackFrameCount.load();
    const auto* constants = g_presentation.constants.Get();
    constexpr float sentinel[4]{0.8F, 0.3F, 0.6F, 1};
    device.context->ClearRenderTargetView(output.rtv.Get(), sentinel);
    FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
    CheckOutputCode(0);
    FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() == generation);
    FO4HDR_WARP_CHECK(g_presentation.constants.Get() == constants);
    FO4HDR_WARP_CHECK(g_presentation.presentationProxy.Get() == proxy.texture.Get());
    FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
    FO4HDR_WARP_CHECK(g_presentation.colorSpaceState == (hdr ? 1 : 0));
    FO4HDR_WARP_CHECK(g_evidence.hdrColorSpaceActive.load() == hdr);
    FO4HDR_WARP_CHECK(!g_evidence.productionActive.load());
    FO4HDR_WARP_CHECK(g_evidence.productionFrameCount.load() == productionFrames);
    FO4HDR_WARP_CHECK(g_evidence.sdrFallbackFrameCount.load() == fallbackFrames + !hdr);
    CompletePresentFrame(false, false);
}

void SharedPresentationContextFixture::CheckOutputCode(std::uint16_t expected, UINT x, UINT y)
{
    FO4HDR_WARP_CHECK(x < kWidth && y < kHeight);
    if (x >= kWidth || y >= kHeight) return;
    device.context->CopyResource(output.staging.Get(), output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read native shared UI presentation output");
    const auto* row = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
    const auto pixel = row[x];
    device.context->Unmap(output.staging.Get(), 0);
    for (UINT channel = 0; channel < 3; ++channel)
        FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>((pixel >> (channel * 10)) & 1023),
            expected) <= 1);
    FO4HDR_WARP_CHECK(pixel >> 30 == 3);
}

void ObservedConstantProbe::Invoke(void* arguments)
{
    auto& probe = *static_cast<ObservedConstantProbe*>(arguments);
    ComPtr<ID3D11Buffer> actual;
    probe.context->PSGetConstantBuffers(kProductionOperatorConstantSlot, 1, &actual);
    FO4HDR_WARP_CHECK(actual.Get() == probe.expected);
    ++probe.calls;
}

void PrepareObservedProductionBindings(Device& device)
{
    g_replacements = {};
    g_replacements.device = device.device.Get();
    g_replacements.configurationReady = true;
    g_replacements.configurationGeneration = g_settingsGeneration;
    g_replacements.configurationKey.mode = 3;
    constexpr std::array programs{&ReplacementShaderSet::toneMapA, &ReplacementShaderSet::toneMapB,
        &ReplacementShaderSet::toneMapNewA, &ReplacementShaderSet::toneMapNewB,
        &ReplacementShaderSet::lut, &ReplacementShaderSet::temporalAa, &ReplacementShaderSet::temporalAa2,
        &ReplacementShaderSet::fxaa, &ReplacementShaderSet::video};
    for (auto program : programs) g_replacements.*program = g_presentation.presentShader;
    g_replacements.operatorConstants = CreateConstantBuffer(*device.device.Get(),
        g_replacements.configurationKey);
    g_replacements.bypassConstants = CreateConstantBuffer(*device.device.Get(), OperatorConstants{});
    const auto lut = CreateIdentityLut(*device.device.Get());
    g_replacements.operatorLut = lut.texture;
    g_replacements.operatorLutSrv = lut.srv;
    g_replacements.operatorLutSampler = CreateClampLinearSampler(*device.device.Get());
    g_replacements.operatorLutKey.algorithm = g_activeSettings.productionAlgorithm;
    g_replacements.operatorLutKey.displayPeakNits = g_activeSettings.displayPeakNits;
    g_replacements.operatorLutKey.toneMapperParameters = ActiveToneMapperParameters(
        g_activeSettings.productionAlgorithm, g_activeSettings.toneMapperParameters);
    FO4HDR_WARP_CHECK(ReplacementSetMatchesConfiguration());
}

Device CreateNativeContextDevice(Device& source)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    CheckHresult(source.device.As(&dxgi), "Get presentation hook adapter device");
    CheckHresult(dxgi->GetAdapter(&adapter), "Get presentation hook adapter");
    Device native;
    constexpr D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    // Debug-layer objects exceed the production callable-tail limit. Use the
    // same adapter's native object and retain the production installation policy.
    CheckHresult(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        0, &requested, 1, D3D11_SDK_VERSION, &native.device, nullptr, &native.context),
        "Create native presentation hook context");
    return native;
}


} // namespace fo4hdr::tests::gpu
