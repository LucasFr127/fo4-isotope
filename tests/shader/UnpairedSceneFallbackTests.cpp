// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "resources/ReplacementShaders.hpp"
#include "GraphicsDiagnostics.hpp"
#include "OperatorConstants.hpp"
#include "RendererLifetime.hpp"
#include "ShaderRegistry.hpp"
#include "SharedUiFrame.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using Pass = RenoDxShaderPass;

// Production replacements are eligible and every draw runs through the context hooks.
// The stand-in stock shaders write SDR code 0.5 onto their targets.
struct UnpairedSceneFixture : SharedPresentationContextFixture
{
    std::vector<ComPtr<ID3D11PixelShader>> stock;
    ComPtr<ID3D11Buffer> nativeConstants;
    Texture source, history;

    explicit UnpairedSceneFixture(Device& value) : SharedPresentationContextFixture(value)
    {
        BindHud();
        PrepareObservedProductionBindings(device);
        g_presentation.colorDomainGeneration = g_evidence.resourceGeneration.load();
        g_activeSettings.uiPaperWhiteNits = 203;
        nativeConstants = CreateConstantBuffer(*device.device.Get(), OperatorConstants{});
        source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        history = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        constexpr float codes[4]{0.5F, 0.5F, 0.5F, 1};
        device.context->ClearRenderTargetView(source.rtv.Get(), codes);
    }

    // The registry keeps raw pointers, so the stand-ins must leave it first.
    ~UnpairedSceneFixture() { ResetShaderRegistry(); }

    static bool Temporal(Pass pass) { return pass == Pass::taa || pass == Pass::taa2; }

    // Temporal stand-ins copy their input into history and the scene target.
    ID3D11PixelShader* CreateStock(Pass pass)
    {
        auto shader = Temporal(pass) ?
            CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs) :
            CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
        auto* raw = stock.emplace_back(std::move(shader)).Get();
        CreatedShaderIdentity{.descriptor = FindShaderDescriptor(pass)}.Register(S_OK, &raw);
        return raw;
    }

    void BindTargets(Pass pass, ID3D11RenderTargetView* output)
    {
        BindHud();
        const bool temporal = Temporal(pass);
        const std::array targets{temporal ? history.rtv.Get() : output, temporal ? output : nullptr};
        device.context->OMSetRenderTargets(temporal ? 2 : 1, targets.data(), nullptr);
        auto* input = source.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        auto* constants = nativeConstants.Get();
        device.context->PSSetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants);
    }

    ID3D11PixelShader* BindReplacement(Pass pass, ID3D11RenderTargetView* output)
    {
        ResetShaderRegistry();
        auto* shader = CreateStock(pass);
        BindTargets(pass, output);
        device.context->PSSetShader(shader, nullptr, 0);
        FO4HDR_WARP_CHECK(record->currentReplacementActive && record->nativePixelShader.Get() == shader);
        return shader;
    }

    void ObservePair() const
    {
        g_heartbeat.Observe(Pass::toneMapA);
        g_heartbeat.Observe(Pass::lut);
    }

    // The stock shader drew in place of its replacement.
    void DrawWithheld(ID3D11PixelShader* shader)
    {
        const auto count = g_evidence.unpairedSceneProducerCount.load();
        {
            NativeReplacementDrawProbe probe(*record, shader, nativeConstants.Get());
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(probe.calls == 1);
        }
        FO4HDR_WARP_CHECK(!record->currentReplacementActive && record->expectedBoundShader == shader);
        FO4HDR_WARP_CHECK(g_evidence.unpairedSceneProducerCount.load() == count + 1);
    }

    // The withheld draw left the whole public image holding game SDR codes.
    void DrawDeclared(ID3D11PixelShader* shader, bool assumed = false)
    {
        DrawWithheld(shader);
        const auto startup = SharedUiStartupPresentation();
        FO4HDR_WARP_CHECK(startup && startup->domain == LinearColorDomain::vanillaSdrCodes);
        FO4HDR_WARP_CHECK(startup && startup->white == 203 && startup->assumed == assumed);
    }

    // A qualified frame leaves public content that a later native write must revoke.
    void PresentQualifiedFrame()
    {
        PrepareScene();
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(true, false);
        FO4HDR_WARP_CHECK(SharedUiPresentationForOutput().has_value());
    }

    void DrawKept()
    {
        const auto count = g_evidence.unpairedSceneProducerCount.load();
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(record->currentReplacementActive);
        FO4HDR_WARP_CHECK(g_evidence.unpairedSceneProducerCount.load() == count);
        FO4HDR_WARP_CHECK(g_productionHealthy.load() && g_referenceHealthy.load());
    }
};

void CheckUnpairedProducers(Device& device)
{
    for (const auto pass : {Pass::taa, Pass::taa2, Pass::fxaa, Pass::lut})
    {
        UnpairedSceneFixture fixture(device);
        fixture.DrawDeclared(fixture.BindReplacement(pass, fixture.proxy.rtv.Get()));
        fixture.PresentReadable(std::pow(0.5, 2.2));
    }
}

// Without a tone map the LUT stays native even onto an internal target, so the TAA that
// reads it writes the game's own SDR codes.
void CheckNativeChain(Device& device)
{
    UnpairedSceneFixture fixture(device);
    fixture.DrawWithheld(fixture.BindReplacement(Pass::lut, fixture.source.rtv.Get()));
    FO4HDR_WARP_CHECK(!SharedUiStartupPresentation());
    fixture.DrawDeclared(fixture.BindReplacement(Pass::taa2, fixture.proxy.rtv.Get()));
    fixture.PresentReadable(std::pow(0.5, 2.2));
}

// Once a tone-map replacement fed the frame, the chain keeps its replacements even with
// the LUT missed, and their public write revokes a declaration. Video needs no tone map.
void CheckKeptReplacements(Device& device)
{
    for (const auto pass : {Pass::taa, Pass::taa2, Pass::fxaa, Pass::lut, Pass::video})
    {
        UnpairedSceneFixture fixture(device);
        FO4HDR_WARP_CHECK(DeclareSharedUiNativeSdr(false));
        if (pass != Pass::video) g_heartbeat.Observe(Pass::toneMapA);
        (void)fixture.BindReplacement(pass, fixture.proxy.rtv.Get());
        fixture.DrawKept();
        FO4HDR_WARP_CHECK(!SharedUiStartupPresentation());
    }
    // A temporal branch before the tone map that writes only internal targets keeps its replacement.
    UnpairedSceneFixture fixture(device);
    const auto internal = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    FO4HDR_WARP_CHECK(DeclareSharedUiNativeSdr(false));
    (void)fixture.BindReplacement(Pass::taa2, internal.rtv.Get());
    fixture.DrawKept();
    FO4HDR_WARP_CHECK(SharedUiStartupPresentation().has_value());
}

// The declaration outranks the linear image an earlier qualified frame left, and the
// pair completing later in the frame publishes its scene instead.
void CheckQualifiedTransitions(Device& device)
{
    {
        UnpairedSceneFixture fixture(device);
        fixture.PresentQualifiedFrame();
        fixture.DrawDeclared(fixture.BindReplacement(Pass::taa2, fixture.proxy.rtv.Get()));
        fixture.PresentReadable(std::pow(0.5, 2.2));
    }
    UnpairedSceneFixture fixture(device);
    fixture.DrawDeclared(fixture.BindReplacement(Pass::taa, fixture.proxy.rtv.Get()));
    fixture.PrepareScene();
    FO4HDR_WARP_CHECK(!SharedUiStartupPresentation());
    FO4HDR_WARP_CHECK(PreparePresentFrame(fixture.chain.Get(), true));
    fixture.CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
    CompletePresentFrame(true, false);
}

// A native optional producer publishing a qualified scene onto the public image ends a
// declaration left by an earlier unpaired frame.
void CheckQualifiedPublishRevokes(Device& device)
{
    UnpairedSceneFixture fixture(device);
    FO4HDR_WARP_CHECK(DeclareSharedUiNativeSdr(false));
    fixture.ObservePair();
    g_scene.currentScene = fixture.source.srv;
    g_scene.currentSceneDomain = LinearColorDomain::linearBt2020UiRelative;
    g_scene.productionOperatorApplied = true;
    g_scene.temporalPassObserved = true;
    g_scene.temporalSceneReady = true;
    fixture.BindHud();
    auto* input = fixture.source.srv.Get();
    device.context->PSSetShaderResources(0, 1, &input);
    fixture.record->currentPass = Pass::dofBlender;
    fixture.record->currentPassKnown = true;
    fixture.record->expectedBoundShader = fixture.hudShader.Get();
    device.context->Draw(3, 0);
    FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == fixture.proxy.srv.Get());
    FO4HDR_WARP_CHECK(!SharedUiStartupPresentation());
}

// A foreign LUT that took its pass through the engine scope is only assumed game output.
void CheckForeignProducer(Device& device)
{
    UnpairedSceneFixture fixture(device);
    ResetShaderRegistry();
    for (const auto pass : {Pass::toneMapNewA, Pass::toneMapNewB, Pass::lut}) (void)fixture.CreateStock(pass);
    const auto foreign = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
    fixture.BindTargets(Pass::lut, fixture.proxy.rtv.Get());
    const auto previous = EnterNativeImageSpacePass(NativeImageSpacePass::lut);
    device.context->PSSetShader(foreign.Get(), nullptr, 0);
    ExitNativeImageSpacePass(previous);
    FO4HDR_WARP_CHECK(fixture.record->currentPassForeign && fixture.record->currentReplacementActive);
    fixture.DrawDeclared(foreign.Get(), true);
    FO4HDR_WARP_CHECK(SharedUiStartupAssumed());
    fixture.PresentReadable(std::pow(0.5, 2.2));
}

// A downstream hook that binds its own shader whenever the original setter runs.
struct DefeatingSetter
{
    ContextRecord& record;
    PsSetShader next;
    ID3D11PixelShader* foreign;
    static inline DefeatingSetter* active{};

    DefeatingSetter(ContextRecord& value, ID3D11PixelShader* shader) :
        record(value), next(value.nextPsSetShader), foreign(shader)
    {
        active = this;
        record.nextPsSetShader = Invoke;
    }
    ~DefeatingSetter()
    {
        record.nextPsSetShader = next;
        active = nullptr;
    }
    DefeatingSetter(const DefeatingSetter&) = delete;
    DefeatingSetter& operator=(const DefeatingSetter&) = delete;

    static void STDMETHODCALLTYPE Invoke(ID3D11DeviceContext* context, ID3D11PixelShader*,
        ID3D11ClassInstance* const* classes, UINT count)
    {
        active->next(context, active->foreign, classes, count);
    }
};

// A partial image, another thread or a defeated restore declares nothing, but still
// revokes the public content an earlier qualified frame left.
void CheckUndeclaredWrites(Device& device)
{
    const auto check = [](std::uint64_t count, std::uint64_t withheld) {
        FO4HDR_WARP_CHECK(g_evidence.unpairedSceneProducerCount.load() == count + withheld);
        FO4HDR_WARP_CHECK(!SharedUiStartupPresentation());
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
    };
    {
        UnpairedSceneFixture fixture(device);
        fixture.PresentQualifiedFrame();
        (void)fixture.BindReplacement(Pass::taa2, fixture.proxy.rtv.Get());
        const D3D11_VIEWPORT half{0, 0, kWidth / 2, kHeight, 0, 1};
        device.context->RSSetViewports(1, &half);
        const auto count = g_evidence.unpairedSceneProducerCount.load();
        device.context->Draw(3, 0);
        check(count, 1);
    }
    {
        UnpairedSceneFixture fixture(device);
        fixture.PresentQualifiedFrame();
        (void)fixture.BindReplacement(Pass::taa2, fixture.proxy.rtv.Get());
        const auto count = g_evidence.unpairedSceneProducerCount.load();
        std::thread([&device] { device.context->Draw(3, 0); }).join();
        check(count, 1);
    }
    UnpairedSceneFixture fixture(device);
    fixture.PresentQualifiedFrame();
    (void)fixture.BindReplacement(Pass::taa2, fixture.proxy.rtv.Get());
    const auto foreign = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs);
    const auto count = g_evidence.unpairedSceneProducerCount.load();
    {
        const DefeatingSetter setter(*fixture.record, foreign.Get());
        device.context->Draw(3, 0);
    }
    FO4HDR_WARP_CHECK(!fixture.record->currentReplacementActive && !fixture.record->currentPassKnown);
    FO4HDR_WARP_CHECK(fixture.record->expectedBoundShader == foreign.Get());
    check(count, 0);
}

// SDR mode keeps its own LUT substitution, and the developer reference path, which has
// no facade, keeps its replacements.
void CheckExclusions(Device& device)
{
    {
        UnpairedSceneFixture fixture(device);
        g_activeSettings = DefaultSdrSettings();
        g_activeSettings.vanillaColorGradingStrengthPercent = 50.0;
        g_sdrModeActive = true;
        FO4HDR_WARP_CHECK(BuildSdrReplacementShaders(device.device.Get()));
        (void)fixture.BindReplacement(Pass::lut, fixture.proxy.rtv.Get());
        fixture.DrawKept();
        g_sdrModeActive = false;
    }
    UnpairedSceneFixture fixture(device);
    g_presentation.applicationFacadeChain = nullptr;
    (void)fixture.BindReplacement(Pass::taa2, fixture.proxy.rtv.Get());
    fixture.DrawKept();
}

}  // namespace

void VerifyUnpairedSceneFallback(Device& device)
{
    auto native = CreateNativeContextDevice(device);
    CheckUnpairedProducers(native);
    CheckNativeChain(native);
    CheckKeptReplacements(native);
    CheckQualifiedTransitions(native);
    CheckQualifiedPublishRevokes(native);
    CheckForeignProducer(native);
    CheckUndeclaredWrites(native);
    CheckExclusions(native);
}

}  // namespace fo4hdr::tests::gpu
