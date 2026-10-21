// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include <iostream>
#include "d3d11/PresentationContextFixture.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/DeviceHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "passes/PresentationPass.hpp"
#include "passes/ObservedDraw.hpp"
#include "passes/ArmorCompositionPass.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include "RendererLifetime.hpp"
#include "PresentFrame.hpp"
#include "SharedUiFrame.hpp"
#include "SharedSdrFrame.hpp"
#include "ShaderRegistry.hpp"
#include "DisplayDiscovery.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "resources/ReplacementShaders.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace fo4hdr::tests::gpu {
namespace {

struct SharedUiContextFixture : SharedPresentationContextFixture
{
    using SharedPresentationContextFixture::SharedPresentationContextFixture;

    void PrepareFlatScene(float white = 203)
    {
        PrepareScene();
        g_scene.temporalPassObserved = false;
        g_scene.temporalSceneReady = false;
        g_activeSettings.uiPaperWhiteNits = white;
        PublishSharedUiScene(CaptureSharedUiWrite(true));
        SetSharedUiPauseResourcesReady(true);
    }

    void DrawFlatMenu(std::uint32_t targetArgument = 0, ArmorUiOwner owner = ArmorUiOwner::flatMenu)
    {
        DrawHud(true, owner, targetArgument);
    }

    void SeedScreenSpace()
    {
        const ArmorUiScope scope({.owner = ArmorUiOwner::screenSpace,
            .threadId = GetCurrentThreadId()});
        FO4HDR_WARP_CHECK(scope.Entered());
    }

    void CheckScreenSpaceLateUi()
    {
        for (const bool backdrop : {false, true})
        {
            PrepareScene();
            SeedScreenSpace();
            BindHud();
            auto shader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
            CreatedShaderIdentity identity;
            identity.lateGameUiComposite = !backdrop;
            identity.lateGameUiBackdrop = backdrop;
            auto* raw = shader.Get();
            identity.Register(S_OK, &raw);
            const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D11_BIND_SHADER_RESOURCE);
            auto* input = source.srv.Get();
            device.context->PSSetShaderResources(0, 1, &input);
            device.context->PSSetShader(shader.Get(), nullptr, 0);
            ExecuteObservedDraw(*record, device.context.Get(),
                {[](void* context) { static_cast<ID3D11DeviceContext*>(context)->Draw(3, 0); },
                    device.context.Get()});
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            const auto linear = std::pow((0.5 + 0.055) / 1.055, 2.4);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linear * 203)));
            CompletePresentFrame(true, false);
            input = nullptr;
            device.context->PSSetShaderResources(0, 1, &input);
            ResetShaderRegistry();
        }
    }

    void CheckScreenSpaceAdmission()
    {
        PrepareFlatScene();
        SeedScreenSpace();
        FO4HDR_WARP_CHECK(SharedUiFrameEncoded());
        DrawFlatMenu();
        PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4));
        PrepareScene();
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        g_scene.currentScene = offscreen.srv;
        SeedScreenSpace();
        FO4HDR_WARP_CHECK(!SharedUiFrameReady() && !SharedUiFrameEncoded());
        device.context->Draw(3, 0);
        SeedScreenSpace();
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        PresentReadable(0.5);
        PrepareScene();
        g_presentation.presentationProxyDomain = LinearColorDomain::unknown;
        SeedScreenSpace();
        FO4HDR_WARP_CHECK(!SharedUiFrameReady() && !SharedUiFrameEncoded());
        PresentReadable(0.5);
        PrepareScene();
        const ArmorUiScopeToken wrongThread{.owner = ArmorUiOwner::screenSpace,
            .threadId = GetCurrentThreadId() + 1, .serial = 1,
            .requestGeneration = ArmorUiScopeGeneration(), .depth = 1};
        SharedUiBoundary(wrongThread, true);
        FO4HDR_WARP_CHECK(SharedUiFrameReady() && !SharedUiFrameEncoded());
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(true, false);
    }

    void CheckStartupHandoff()
    {
        const auto productionFrames = g_evidence.productionFrameCount.load();
        const auto fallbackFrames = g_evidence.sdrFallbackFrameCount.load();
        // The fixture supplies accepted output evidence; physical display qualification is separate.
        g_presentation.colorSpaceState = 1;
        g_evidence.hdrColorSpaceActive = true;
        for (const float white : {100.0F, 203.0F, 300.0F})
        {
            g_activeSettings.uiPaperWhiteNits = white;
            FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
            FO4HDR_WARP_CHECK(SharedUiStartupSdr());
            g_scene = {};
            SeedScreenSpace();
            FO4HDR_WARP_CHECK(SharedUiStartupSdr() && !SharedUiFrameEncoded());
            g_activeSettings.uiPaperWhiteNits = white * 2;
            for (const float code : {0.0F, 0.25F, 0.5F, 1.0F, 1.25F})
            {
                const float sdr[4]{code, code, code, 1};
                constexpr float sentinel[4]{0.8F, 0.3F, 0.6F, 1};
                device.context->ClearRenderTargetView(proxy.rtv.Get(), sdr);
                device.context->ClearRenderTargetView(output.rtv.Get(), sentinel);
                FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
                CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(std::pow(code, 2.2) * white)));
                FO4HDR_WARP_CHECK(g_presentation.colorSpaceState == 1 &&
                    g_evidence.hdrColorSpaceActive.load());
                FO4HDR_WARP_CHECK(!g_evidence.productionActive.load());
                FO4HDR_WARP_CHECK(g_evidence.productionFrameCount.load() == productionFrames);
                FO4HDR_WARP_CHECK(g_evidence.sdrFallbackFrameCount.load() == fallbackFrames);
                CompletePresentFrame(false, false);
            }
        }
        BindHud();
        record->currentReplacementActive = true;
        device.context->Draw(3, 0);
        record->currentReplacementActive = false;
        FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
        CheckFrozenWhitePresentation();
    }

    void CheckFlatWhitePresentation()
    {
        for (const float white : {100.0F, 203.0F, 300.0F})
        {
            PrepareFlatScene(white);
            DrawFlatMenu();
            g_activeSettings.uiPaperWhiteNits = white * 2;
            PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4), white);
        }
    }

    void CheckSceneOnlyPresentation()
    {
        for (const bool temporal : {true, false})
        {
            PrepareScene();
            g_scene.temporalPassObserved = temporal;
            g_scene.temporalSceneReady = temporal;
            SetSharedUiPauseResourcesReady(false);
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && !SharedUiFrameEncoded());
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
            FO4HDR_WARP_CHECK(!SharedUiFrameEncoded());
            CompletePresentFrame(true, false);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        }
    }

    void CheckNonTemporalScopes()
    {
        for (const auto owner : {ArmorUiOwner::hud, ArmorUiOwner::flatMenu,
                 ArmorUiOwner::screenSpace, ArmorUiOwner::powerArmorRain, ArmorUiOwner::workshop})
        {
            PrepareFlatScene();
            SetSharedUiPauseResourcesReady(false);
            DrawHud(true, owner);
            PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4));
        }
    }

    void CheckEmptyNativeScopes()
    {
        for (const auto owner : {ArmorUiOwner::hud, ArmorUiOwner::flatMenu, ArmorUiOwner::screenSpace})
        {
            PrepareScene();
            BindHud();
            {
                const ArmorUiScope scope({.owner = owner, .threadId = GetCurrentThreadId(),
                    .targetArgument = 0, .postAa = owner != ArmorUiOwner::screenSpace,
                    .hasRoot = owner != ArmorUiOwner::screenSpace});
                FO4HDR_WARP_CHECK(scope.Entered() && SharedUiFrameEncoded());
                FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            }
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            SeedScreenSpace();
            CheckEncodedSeed(proxy);
            g_activeSettings.uiPaperWhiteNits = 406;
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
            CompletePresentFrame(true, false);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            InvalidateSharedUiPresentation();
            FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
            PresentReadable(0.5);
        }
    }

    void CheckSceneOnlyMutations()
    {
        ComPtr<ID3D11RenderTargetView> alias;
        CheckHresult(device.device->CreateRenderTargetView(proxy.texture.Get(), nullptr, &alias),
            "Create scene-only presentation alias");
        PrepareScene();
        constexpr float white[4]{1, 1, 1, 1};
        device.context->ClearRenderTargetView(alias.Get(), white);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput()->assumed);
        PresentReadable(1);
        PrepareScene();
        device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
        FO4HDR_WARP_CHECK(SharedUiFrameReady());
        device.context->CopyResource(proxy.texture.Get(), proxy.staging.Get());
        FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput()->assumed);
        PresentReadable(0.5);
        PrepareScene();
        BindHud();
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        PresentReadable(0.5);
        CheckSceneOnlyPresentation();
    }

    void CheckSceneOnlyEligibility()
    {
        PrepareScene();
        g_scene.temporalSceneReady = false;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        g_scene.temporalSceneReady = true;
        g_presentation.presentationProxyDomain = LinearColorDomain::encodedSrgbBt709UiRelative;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        g_presentation.presentationProxyDomain = LinearColorDomain::linearBt2020UiRelative;
        ++g_presentation.colorDomainGeneration;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        --g_presentation.colorDomainGeneration;
        const auto generation = record->generation;
        record->generation = 0;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        record->generation = generation;
        g_armor.armorEncodedActive = true;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        g_armor.armorEncodedActive = false;
        {
            const ArmorUiScope scope({.owner = ArmorUiOwner::other,
                .threadId = GetCurrentThreadId(), .postAa = false, .hasRoot = true});
            FO4HDR_WARP_CHECK(scope.Entered() && !SharedUiFrameReady());
        }
        FO4HDR_WARP_CHECK(SharedUiFrameReady());
        CompletePresentFrame(false, false);
    }

    void CheckSceneOnlyOffscreenDraw()
    {
        PrepareScene();
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        BindHud();
        auto* target = offscreen.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(SharedUiFrameReady());
        g_scene.currentScene = offscreen.srv;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        g_scene.currentScene = proxy.srv;
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(true, false);
    }

    void CheckNativeQueries()
    {
        for (const auto owner : {ArmorUiOwner::hud, ArmorUiOwner::screenSpace})
        {
            const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION, 0};
            ComPtr<ID3D11Query> query;
            CheckHresult(device.device->CreateQuery(&description, &query), "Create native shared UI query");
            PrepareScene();
            DrawHud(false, owner);
            device.context->Begin(query.Get());
            FO4HDR_WARP_CHECK(!record->computeQueries.CanDispatch());
            DrawFlatMenu();
            device.context->End(query.Get());
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            const auto linearHud = std::pow((0.5 + 0.055) / 1.055, 2.4);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linearHud * 203)));
            CompletePresentFrame(true, false);
            PrepareScene();
            device.context->Begin(query.Get());
            DrawHud(true, owner);
            device.context->End(query.Get());
            FO4HDR_WARP_CHECK(!SharedUiFrameReady() && !SharedUiFrameEncoded());
            PresentReadable(0.5);
            CheckFrozenWhitePresentation();
        }
    }

    void CheckSequentialNativeScopes()
    {
        PrepareScene();
        for (const auto owner : {ArmorUiOwner::screenSpace, ArmorUiOwner::powerArmorRain,
                 ArmorUiOwner::hud, ArmorUiOwner::flatMenu, ArmorUiOwner::workshop})
        {
            BindHud();
            {
                const ArmorUiScope scope({.owner = owner, .threadId = GetCurrentThreadId(),
                    .postAa = owner != ArmorUiOwner::screenSpace,
                    .hasRoot = owner != ArmorUiOwner::screenSpace});
                FO4HDR_WARP_CHECK(scope.Entered() && SharedUiScopeActive(CurrentArmorUiScope()));
                FO4HDR_WARP_CHECK(!SharedUiFrameReady());
                FO4HDR_WARP_CHECK(SharedUiFrameEncoded() && SharedUiPaperWhite() == 203);
                ComPtr<ID3D11RenderTargetView> target;
                device.context->OMGetRenderTargets(1, &target, nullptr);
                FO4HDR_WARP_CHECK(target.Get() == proxy.rtv.Get());
                device.context->Draw(3, 0);
            }
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            g_activeSettings.uiPaperWhiteNits = 406;
        }
        g_armor.armorEncodedActive = true;
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        g_armor.armorEncodedActive = false;
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        const auto linear = std::pow((0.5 + 0.055) / 1.055, 2.4);
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linear * 203)));
        CompletePresentFrame(true, false);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
    }

    static void ArmorBoundary(const ArmorUiScopeToken& token, bool entering) noexcept
    {
        if (entering) SharedUiBoundary(token, true);
        ArmorEncodedBoundary(token, entering);
        if (!entering) SharedUiBoundary(token, false);
    }

    void CheckUnrelatedArmorWork()
    {
        for (const bool write : {false, true})
        {
            const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            CreatedShaderIdentity identity{.descriptor = FindShaderDescriptor(RenoDxShaderPass::lut)};
            auto* shader = hudShader.Get();
            identity.Register(S_OK, &shader);
            {
                const ArmorUiScope nested({.owner = ArmorUiOwner::other,
                    .threadId = GetCurrentThreadId(), .postAa = false, .hasRoot = true});
                FO4HDR_WARP_CHECK(nested.Entered() && g_armor.armorEncodedActive);
                auto* target = offscreen.rtv.Get();
                device.context->OMSetRenderTargets(1, &target, nullptr);
                device.context->PSSetShader(shader, nullptr, 0);
                FO4HDR_WARP_CHECK(g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
                if (write) device.context->Draw(3, 0);
                FO4HDR_WARP_CHECK(g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
            }
            ResetShaderRegistry();
            BindHud();
        }
    }

    void PrepareArmorResources()
    {
        device.context->OMSetRenderTargets(0, nullptr, nullptr);
        RetireSharedUiPublicImage();
        proxy = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        g_presentation.presentationProxy = proxy.texture;
        g_presentation.presentationProxyRtv = proxy.rtv;
        g_presentation.presentationProxySrv = proxy.srv;
        g_presentation.presentationProxyUav = proxy.uav;
        FO4HDR_WARP_CHECK(EnsureSharedUiResources(device.device.Get()));
        ResetArmorCompositionResources();
        EnsureArmorEncodedResources(device.device.Get(), device.context.Get());
        FO4HDR_WARP_CHECK(g_armor.armorEncodedShader != nullptr);
    }

    void CheckArmorSharedScope()
    {
        for (const bool write : {false, true})
        for (const bool temporal : {false, true})
        {
            PrepareArmorResources();
            SetArmorUiBoundaryCallback(ArmorBoundary);
            PrepareScene();
            g_scene.temporalPassObserved = temporal;
            g_scene.temporalSceneReady = temporal;
            SetSharedUiPauseResourcesReady(false);
            BindHud();
            const auto mismatches = g_evidence.colorDomainMismatchCount.load();
            {
                const ArmorUiScope scope({.owner = ArmorUiOwner::powerArmor,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                FO4HDR_WARP_CHECK(scope.Entered());
                FO4HDR_WARP_CHECK(SharedUiScopeActive(CurrentArmorUiScope()));
                FO4HDR_WARP_CHECK(g_armor.armorEncodedActive);
                FO4HDR_WARP_CHECK(!SharedUiFrameReady());
                CheckUnrelatedArmorWork();
                if (write) device.context->Draw(3, 0);
            }
            // Native armor binds of the gamma 2.2 proxy are the bridge, not a domain conflict.
            FO4HDR_WARP_CHECK(g_evidence.colorDomainMismatchCount.load() == mismatches);
            FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
            DrawHud(write);
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiPaperWhite() == 203);
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            const auto linearHud = write ? std::pow((0.5 + 0.055) / 1.055, 2.4) : 0.5;
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linearHud * 203)));
            CompletePresentFrame(true, false);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            SetArmorUiBoundaryCallback(SharedUiBoundary);
        }
    }

    void CheckArmorDrawConflict()
    {
        PrepareArmorResources();
        SetArmorUiBoundaryCallback(ArmorBoundary);
        CheckNestedScope(ArmorUiOwner::powerArmor, true);
        FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
        CheckArmorFreshFrame();
        SetArmorUiBoundaryCallback(SharedUiBoundary);
    }

    void CheckArmorFreshFrame()
    {
        FO4HDR_WARP_CHECK(g_referenceHealthy.load() && !g_armor.armorEncodedFailed);
        PrepareScene();
        DrawHud(false, ArmorUiOwner::powerArmor);
        FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
        PresentEncodedUi(0.5);
    }

    void CheckArmorCrossPresentRecovery()
    {
        PrepareArmorResources();
        SetArmorUiBoundaryCallback(ArmorBoundary);
        PrepareScene();
        BindHud();
        {
            const ArmorUiScope scope({.owner = ArmorUiOwner::powerArmor,
                .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
            FO4HDR_WARP_CHECK(scope.Entered() && g_armor.armorEncodedActive);
            PresentReadable(0.5);
            FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
        }
        CheckArmorFreshFrame();
        SetArmorUiBoundaryCallback(SharedUiBoundary);
    }

    void CheckArmorRestoreFailure(bool contextUnsafe)
    {
        PrepareArmorResources();
        SetArmorUiBoundaryCallback(ArmorBoundary);
        PrepareScene();
        BindHud();
        const auto decodeConstants = g_armor.armorEncodedConstants[0];
        std::array<std::array<std::uint16_t, 4>, kWidth> encoded{};
        {
            const ArmorUiScope scope({.owner = ArmorUiOwner::powerArmor,
                .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
            FO4HDR_WARP_CHECK(scope.Entered() && g_armor.armorEncodedActive);
            encoded = ReadPublicPixels();
            if (contextUnsafe) g_rendererDevice.activeContext = nullptr;
            else g_armor.armorEncodedConstants[0].Reset();
            CloseArmorEncodedCompositionForLifecycle();
            g_rendererDevice.activeContext = device.context.Get();
            g_armor.armorEncodedConstants[0] = decodeConstants;
        }
        FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && g_armor.armorEncodedFailed);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load() && !SharedUiFrameReady());
        const auto retained = SharedUiEmergencyPresentation();
        FO4HDR_WARP_CHECK(retained && retained->domain == LinearColorDomain::encodedGamma22Bt709UiRelative);
        PresentReadable(std::pow(Binary16ToFloat(encoded[0][0]), 2.2));
        FO4HDR_WARP_CHECK(ReadPublicPixels() == encoded);
        PrepareScene();
        FO4HDR_WARP_CHECK(SharedUiFrameReady() && g_armor.armorEncodedFailed);
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(true, false);
        SetArmorUiBoundaryCallback(SharedUiBoundary);
    }

    void CheckArmorCommandListRecovery()
    {
        PrepareArmorResources();
        SetArmorUiBoundaryCallback(ArmorBoundary);
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ComPtr<ID3D11DeviceContext> deferred;
        CheckHresult(device.device->CreateDeferredContext(0, &deferred),
            "Create armor command-list context");
        deferred->CopyResource(offscreen.texture.Get(), proxy.texture.Get());
        ComPtr<ID3D11CommandList> commands;
        CheckHresult(deferred->FinishCommandList(FALSE, &commands),
            "Finish armor public-image read command list");
        const auto generation = g_evidence.resourceGeneration.load();
        for (const bool restore : {false, true})
        {
            PrepareScene();
            BindHud();
            {
                const ArmorUiScope scope({.owner = ArmorUiOwner::powerArmor,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                FO4HDR_WARP_CHECK(scope.Entered() && g_armor.armorEncodedActive);
                device.context->ExecuteCommandList(commands.Get(), restore);
                FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
                FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
                CheckEncodedSeed(offscreen);
            }
            FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() == generation);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput());
            PresentReadable(0.5);
            CheckArmorFreshFrame();
        }
        SetArmorUiBoundaryCallback(SharedUiBoundary);
    }

    void CheckArmorReadConflicts()
    {
        for (const bool copy : {false, true})
        {
            PrepareArmorResources();
            SetArmorUiBoundaryCallback(ArmorBoundary);
            PrepareScene();
            BindHud();
            const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            const auto blur = CreateConstantBuffer(*device.device.Get(), HudConstants{.operation = 2});
            {
                const ArmorUiScope outer({.owner = ArmorUiOwner::powerArmor,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                const ArmorUiScope nested({.owner = ArmorUiOwner::other,
                    .threadId = GetCurrentThreadId(), .hasRoot = true});
                FO4HDR_WARP_CHECK(outer.Entered() && nested.Entered());
                auto* target = offscreen.rtv.Get();
                device.context->OMSetRenderTargets(1, &target, nullptr);
                if (copy)
                    device.context->CopyResource(offscreen.texture.Get(), proxy.texture.Get());
                else
                {
                    auto* input = proxy.srv.Get();
                    auto* constants = blur.Get();
                    device.context->PSSetShaderResources(0, 1, &input);
                    device.context->PSSetConstantBuffers(0, 1, &constants);
                    device.context->Draw(3, 0);
                    input = nullptr;
                    device.context->PSSetShaderResources(0, 1, &input);
                }
            }
            FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            CheckEncodedSeed(offscreen);
            PresentReadable(0.5);
            CheckArmorFreshFrame();
        }
        ResetArmorCompositionResources();
        EnsureArmorEncodedResources(device.device.Get(), device.context.Get());
        SetArmorUiBoundaryCallback(SharedUiBoundary);
    }

    void CheckArmorPauseOrder(ArmorUiOwner first, ArmorUiOwner second)
    {
        for (const bool write : {false, true})
        {
            PrepareFlatScene();
            DrawHud(first == ArmorUiOwner::flatMenu || write, first);
            FO4HDR_WARP_CHECK(!g_armor.armorEncodedActive && !g_armor.armorEncodedFailed);
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
            FO4HDR_WARP_CHECK(SharedUiPaperWhite() == 203);
            g_activeSettings.uiPaperWhiteNits = 406;
            DrawHud(second == ArmorUiOwner::flatMenu || write, second);
            auto linear = std::pow((0.5 + 0.055) / 1.055, 2.4);
            if (write && second == ArmorUiOwner::powerArmor) linear = std::pow(0.5, 2.2);
            PresentEncodedUi(linear);
            CheckFrozenWhitePresentation();
        }
    }

    void CheckArmorPauseScopes()
    {
        SetArmorUiBoundaryCallback(ArmorBoundary);
        for (const auto owner : {ArmorUiOwner::powerArmor, ArmorUiOwner::powerArmorRain})
        {
            CheckArmorPauseOrder(owner, ArmorUiOwner::flatMenu);
            CheckArmorPauseOrder(ArmorUiOwner::flatMenu, owner);
            CheckFlatMenuAdmission(owner);
        }
        SetArmorUiBoundaryCallback(SharedUiBoundary);
    }

    void CheckFlatMenuBindBeforeScene()
    {
        PrepareFlatScene();
        g_scene = {};
        g_presentation.presentationProxyDomain = LinearColorDomain::unknown;
        const auto generation = g_evidence.resourceGeneration.load();
        g_heartbeat.BeginFrame(generation, g_frame);
        g_heartbeat.Observe(RenoDxShaderPass::toneMapA);
        PrepareObservedProductionBindings(device);
        BindHud();
        FO4HDR_WARP_CHECK(g_scene.currentScene == nullptr);
        record->currentPass = RenoDxShaderPass::lut;
        record->currentPassKnown = true;
        record->currentReplacementActive = true;
        record->expectedBoundShader = hudShader.Get();
        ObservedConstantProbe probe{device.context.Get(), g_replacements.operatorConstants.Get()};
        ExecuteObservedDraw(*record, device.context.Get(), {ObservedConstantProbe::Invoke, &probe});
        FO4HDR_WARP_CHECK(probe.calls == 1 && g_scene.productionOperatorApplied);
        FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
        device.context->PSSetShader(hudShader.Get(), nullptr, 0);
        {
            // The native writer retains the public target bound before LUT selected its scene.
            const ArmorUiScope scope({.owner = ArmorUiOwner::flatMenu, .threadId = GetCurrentThreadId(),
                .targetArgument = 0, .postAa = true, .hasRoot = true});
            FO4HDR_WARP_CHECK(scope.Entered() && SharedUiFrameEncoded());
            device.context->Draw(3, 0);
        }
        FO4HDR_WARP_CHECK(SharedUiFrameReady());
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        const auto linearMenu = std::pow((0.5 + 0.055) / 1.055, 2.4);
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linearMenu * 203)));
        CompletePresentFrame(true, false);
    }

    void PrepareInternalTemporalScene(const Texture& source)
    {
        PrepareScene();
        g_activeSettings.uiPaperWhiteNits = 406;
        PrepareObservedProductionBindings(device);
        g_replacements.configurationKey.uiTransportWhiteNits = 406;
        g_replacements.operatorConstants = CreateConstantBuffer(*device.device.Get(),
            g_replacements.configurationKey);
        constexpr float scene[4]{0.5F, 0.5F, 0.5F, 1};
        device.context->ClearRenderTargetView(source.rtv.Get(), scene);
        g_scene.currentScene = source.srv;
        g_scene.temporalPassObserved = false;
        g_scene.temporalSceneReady = false;
        PublishSharedUiScene(CaptureSharedUiWrite(true));
    }

    void ObserveSyntheticTemporal(RenoDxShaderPass pass, const Texture& source,
        const Texture& target, ID3D11Buffer* expectedConstants)
    {
        D3D11_TEXTURE2D_DESC description{};
        target.texture->GetDesc(&description);
        const auto history = CreateSizedTexture(*device.device.Get(),
            {description.Width, description.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE});
        const auto temporal = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs);
        BindHud();
        const std::array targets{history.rtv.Get(), target.rtv.Get()};
        device.context->OMSetRenderTargets(2, targets.data(), nullptr);
        device.context->PSSetShader(temporal.Get(), nullptr, 0);
        auto* input = source.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        record->currentPass = pass;
        record->currentPassKnown = true;
        record->currentReplacementActive = true;
        record->expectedBoundShader = temporal.Get();
        ObservedConstantProbe probe{device.context.Get(), expectedConstants};
        ExecuteObservedDraw(*record, device.context.Get(),
            {[](void* arguments) {
                auto* observed = static_cast<ObservedConstantProbe*>(arguments);
                ObservedConstantProbe::Invoke(observed);
                FindContext(observed->context)->nextDraw(observed->context, 3, 0);
            }, &probe});
        FO4HDR_WARP_CHECK(probe.calls == 1);
        input = nullptr;
        device.context->PSSetShaderResources(0, 1, &input);
    }

    void CheckInternalTemporalContinuation()
    {
        const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto internal = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto intermediateSized = CreateSizedTexture(*device.device.Get(),
            {kWidth * 2, kHeight * 2, DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE});
        const auto copy = CreateConstantBuffer(*device.device.Get(), HudConstants{.operation = 4});
        const auto foreign = CreateConstantBuffer(*device.device.Get(),
            HudConstants{.color = {0.25F, 0.25F, 0.25F, 1}, .rectangle = {1, 0, kWidth, kHeight}});
        for (const auto pass : {RenoDxShaderPass::taa, RenoDxShaderPass::taa2})
        for (const bool differentSize : {false, true})
        {
            PrepareInternalTemporalScene(source);
            const auto& selected = differentSize ? intermediateSized : internal;
            ObserveSyntheticTemporal(pass, source, selected, g_replacements.operatorConstants.Get());
            FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), selected.srv.Get()));
            FO4HDR_WARP_CHECK(g_scene.temporalSceneReady && g_scene.productionOperatorApplied);
            FO4HDR_WARP_CHECK(g_scene.currentSceneDomain == LinearColorDomain::linearBt2020UiRelative);
            if (differentSize)
            {
                DrawHud(false);
                FO4HDR_WARP_CHECK(!SharedUiFrameEncoded() && g_referenceHealthy.load());
                FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), selected.srv.Get()));
            }
            BindHud();
            auto* input = selected.srv.Get();
            auto* constants = copy.Get();
            device.context->PSSetShaderResources(0, 1, &input);
            device.context->PSSetConstantBuffers(0, 1, &constants);
            record->currentPass = RenoDxShaderPass::dofBlender;
            record->currentPassKnown = true;
            record->expectedBoundShader = hudShader.Get();
            device.context->Draw(3, 0);
            input = nullptr;
            device.context->PSSetShaderResources(0, 1, &input);
            FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
            FO4HDR_WARP_CHECK(g_scene.temporalSceneReady);
            SeedScreenSpace();
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
            FO4HDR_WARP_CHECK(SharedUiPaperWhite() == 406);
            BindHud();
            constants = foreign.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(203)));
            const auto foreignCode = QuantizeR10Unorm(EncodeSt2084Pq(
                std::pow((0.25 + 0.055) / 1.055, 2.4) * 406));
            CheckOutputCode(foreignCode, 1);
            const auto expected = ReadPresentationPixel(output.texture.Get());
            CompletePresentFrame(true, false);
            g_activeSettings.uiPaperWhiteNits = 812;
            PresentReadable(0.5, 406);
            FO4HDR_WARP_CHECK(ReadPresentationPixel(output.texture.Get()) == expected);
            CheckOutputCode(foreignCode, 1);
            CheckFrozenWhitePresentation();
        }
    }

    void CheckInternalTemporalRejections()
    {
        const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto unrelated = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto internal = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto invalid = CreateTexture(*device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        constexpr float other[4]{0.25F, 0.25F, 0.25F, 1};
        device.context->ClearRenderTargetView(unrelated.rtv.Get(), other);
        for (const auto pass : {RenoDxShaderPass::taa, RenoDxShaderPass::taa2})
        for (const bool wrongInput : {false, true})
        {
            PrepareInternalTemporalScene(source);
            ObserveSyntheticTemporal(pass, wrongInput ? unrelated : source,
                wrongInput ? internal : invalid, g_replacements.bypassConstants.Get());
            FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == source.srv.Get());
            FO4HDR_WARP_CHECK(!g_scene.temporalPassObserved && !g_scene.temporalSceneReady);
            FO4HDR_WARP_CHECK(g_referenceHealthy.load());
            CompletePresentFrame(false, false);
        }
    }

    void CheckFlatMenuAdmission(ArmorUiOwner owner = ArmorUiOwner::flatMenu)
    {
        PrepareFlatScene();
        DrawFlatMenu(1, owner);
        if (owner == ArmorUiOwner::flatMenu || owner == ArmorUiOwner::workshop)
        {
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            PresentReadable(0.5);
        }
        else
            PresentEncodedUi(owner == ArmorUiOwner::powerArmor ? std::pow(0.5, 2.2) :
                std::pow((0.5 + 0.055) / 1.055, 2.4));
        PrepareFlatScene();
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        g_scene.currentScene = offscreen.srv;
        DrawFlatMenu(0, owner);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        PresentReadable(0.5);
        PrepareFlatScene();
        g_scene.temporalPassObserved = true;
        DrawFlatMenu(0, owner);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        // Admission rejects these existing hard-failure conditions before Present handles them.
        CompletePresentFrame(false, false);
        PrepareFlatScene();
        g_scene.currentSceneDomain = LinearColorDomain::linearBt709Relative;
        g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
        DrawFlatMenu(0, owner);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        CompletePresentFrame(false, false);
    }

    void CheckFlatMenuMutations(ArmorUiOwner owner = ArmorUiOwner::flatMenu)
    {
        ComPtr<ID3D11RenderTargetView> alias;
        CheckHresult(device.device->CreateRenderTargetView(proxy.texture.Get(), nullptr, &alias),
            "Create pause-menu presentation alias");
        PrepareFlatScene();
        constexpr float white[4]{1, 1, 1, 1};
        device.context->ClearRenderTargetView(alias.Get(), white);
        DrawFlatMenu(0, owner);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput()->assumed);
        PresentReadable(0.5);
        PrepareFlatScene();
        ComPtr<ID3D11Resource> aliased;
        alias->GetResource(&aliased);
        device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
        device.context->CopyResource(aliased.Get(), proxy.staging.Get());
        DrawFlatMenu(0, owner);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput()->assumed);
        PresentReadable(0.5);
        PrepareFlatScene();
        BindHud();
        auto* target = alias.Get();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        device.context->Draw(3, 0);
        DrawFlatMenu(0, owner);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        PresentReadable(0.5);
        CheckFlatWhitePresentation();
    }

    void CheckFlatMenuScopeRetirement(ArmorUiOwner owner = ArmorUiOwner::flatMenu)
    {
        PrepareFlatScene();
        BindHud();
        {
            const ArmorUiScope scope({.owner = owner, .threadId = GetCurrentThreadId(),
                .targetArgument = 0, .postAa = true, .hasRoot = true});
            FO4HDR_WARP_CHECK(scope.Entered() && SharedUiFrameEncoded());
            device.context->Draw(3, 0);
            ResetArmorUiScope();
        }
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        PresentReadable(std::pow((0.5 + 0.055) / 1.055, 2.4));
        CheckFlatWhitePresentation();
    }

    void CheckNativeScopeGuards(ArmorUiOwner owner)
    {
        std::array<ArmorUiScopeToken, 5> invalid;
        invalid.fill({.owner = owner, .threadId = GetCurrentThreadId(), .serial = 1,
            .requestGeneration = ArmorUiScopeGeneration(), .depth = 1,
            .postAa = true, .hasRoot = true});
        invalid[0].postAa = false;
        invalid[1].hasRoot = false;
        ++invalid[2].threadId;
        ++invalid[3].requestGeneration;
        invalid[4].depth = 2;
        for (const auto& token : invalid)
        {
            PrepareFlatScene();
            SharedUiBoundary(token, true);
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && !SharedUiFrameEncoded());
            CompletePresentFrame(false, false);
        }
    }

    void CheckWorkshopSequence(std::initializer_list<ArmorUiOwner> owners)
    {
        PrepareFlatScene();
        const auto saved = hudConstants;
        hudConstants = CreateConstantBuffer(*device.device.Get(),
            HudConstants{.rectangle = {1, 0, kWidth, kHeight}});
        const auto continuation = CreateConstantBuffer(*device.device.Get(),
            HudConstants{.rectangle = {2, 0, kWidth, kHeight}});
        for (const auto owner : owners)
        {
            DrawHud(true, owner);
            hudConstants = continuation;
            FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
            g_activeSettings.uiPaperWhiteNits = 406;
        }
        hudConstants = saved;
        PresentEncodedUi(0.5);
        const auto linearUi = std::pow((0.5 + 0.055) / 1.055, 2.4);
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(linearUi * 203)), 1);
    }

    void CheckRejectedFrames()
    {
        for (const auto owner : {ArmorUiOwner::hud, ArmorUiOwner::screenSpace})
        {
            constexpr float white[4]{1, 1, 1, 1};
            PrepareScene();
            DrawHud(true, owner);
            InvalidateSharedUiPresentation();
            FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
            PresentReadable(std::pow((0.5 + 0.055) / 1.055, 2.4));
            DrawHud(true, owner);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            PresentReadable(std::pow((0.5 + 0.055) / 1.055, 2.4));
            PrepareScene();
            DrawHud(false, owner);
            device.context->ClearRenderTargetView(proxy.rtv.Get(), white);
            FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput()->assumed);
            PresentReadable(1);
            PrepareScene();
            DrawHud(true, owner);
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            ObserveSharedUiDraw({.currentPrimaryRtv = proxy.rtv.Get(), .currentPassKnown = true});
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            PresentReadable(std::pow((0.5 + 0.055) / 1.055, 2.4));
            PrepareScene();
            DrawHud(true, owner);
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
            device.context->CopyResource(proxy.texture.Get(), proxy.staging.Get());
            FO4HDR_WARP_CHECK(!SharedUiFrameReady() && SharedUiPresentationForOutput()->assumed);
            PresentReadable(std::pow((0.5 + 0.055) / 1.055, 2.4));
        }
    }

    void CheckSemanticReset()
    {
        SetSharedUiPauseResourcesReady(true);
        ResetResources(true, true);
        FO4HDR_WARP_CHECK(SharedUiPauseResourcesReady());
        FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
        FO4HDR_WARP_CHECK(g_presentation.presentationProxy.Get() == proxy.texture.Get());
        FO4HDR_WARP_CHECK(g_presentation.presentationProxyRtv.Get() == proxy.rtv.Get());
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        PresentReadable(0.5);
    }

    void CheckResourceRetirement()
    {
        SetSharedUiPauseResourcesReady(true);
        ResetResources();
        FO4HDR_WARP_CHECK(!SharedUiPauseResourcesReady());
        FO4HDR_WARP_CHECK(!SharedUiEmergencyPresentation());
    }

    void CheckNestedScope(ArmorUiOwner owner, bool write)
    {
        PrepareScene();
        BindHud();
        {
            const ArmorUiScope outer({.owner = owner, .threadId = GetCurrentThreadId(),
                .postAa = owner != ArmorUiOwner::screenSpace,
                .hasRoot = owner != ArmorUiOwner::screenSpace});
            FO4HDR_WARP_CHECK(outer.Entered());
            device.context->Draw(3, 0);
            const ArmorUiScope nested({.owner = ArmorUiOwner::other, .threadId = GetCurrentThreadId(),
                .postAa = false, .hasRoot = true});
            FO4HDR_WARP_CHECK(nested.Entered());
            if (write) device.context->Draw(3, 0);
        }
        if (write && owner == ArmorUiOwner::powerArmor)
        {
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            PresentReadable(std::pow((0.5 + 0.055) / 1.055, 2.4));
        }
        else
            PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4));
    }

    void CheckUnrelatedScope()
    {
        for (const bool seeded : {false, true})
        {
            PrepareScene();
            if (seeded) DrawHud(false);
            const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            BindHud();
            auto* target = offscreen.rtv.Get();
            device.context->OMSetRenderTargets(1, &target, nullptr);
            {
                const ArmorUiScope scope({.owner = ArmorUiOwner::other,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                FO4HDR_WARP_CHECK(scope.Entered());
                device.context->Draw(3, 0);
            }
            DrawHud(false);
            PresentEncodedUi(0.5);
        }
    }

};

} // namespace

void VerifySharedUiContext(Device& device)
{
    for (const bool contextUnsafe : {false, true})
    {
        SharedUiContextFixture failure(device);
        failure.CheckArmorRestoreFailure(contextUnsafe);
    }
    SharedUiContextFixture fixture(device);
    fixture.CheckStartupHandoff();
    fixture.CheckFrozenWhitePresentation();
    fixture.CheckFlatWhitePresentation();
    fixture.CheckFrozenWhitePresentation();
    fixture.PrepareScene();
    SetSharedUiPauseResourcesReady(false);
    fixture.DrawFlatMenu();
    fixture.PresentEncodedUi(std::pow((0.5 + 0.055) / 1.055, 2.4));
    fixture.CheckScreenSpaceLateUi();
    fixture.CheckScreenSpaceAdmission();
    fixture.CheckSceneOnlyPresentation();
    fixture.CheckNonTemporalScopes();
    fixture.CheckSceneOnlyMutations();
    fixture.CheckSceneOnlyEligibility();
    fixture.CheckSceneOnlyOffscreenDraw();
    fixture.CheckNativeQueries();
    fixture.CheckFlatMenuBindBeforeScene();
    fixture.CheckInternalTemporalContinuation();
    fixture.CheckInternalTemporalRejections();
    for (const auto owner : {ArmorUiOwner::flatMenu, ArmorUiOwner::workshop})
    {
        fixture.CheckFlatMenuAdmission(owner);
        fixture.CheckFlatMenuMutations(owner);
        fixture.CheckFlatMenuScopeRetirement(owner);
        fixture.CheckNativeScopeGuards(owner);
    }
    fixture.CheckWorkshopSequence({ArmorUiOwner::workshop, ArmorUiOwner::flatMenu,
        ArmorUiOwner::screenSpace});
    fixture.CheckWorkshopSequence({ArmorUiOwner::workshop, ArmorUiOwner::hud, ArmorUiOwner::flatMenu,
        ArmorUiOwner::screenSpace});
    fixture.CheckWorkshopSequence({ArmorUiOwner::workshop, ArmorUiOwner::screenSpace});
    fixture.CheckWorkshopSequence({ArmorUiOwner::workshop, ArmorUiOwner::hud,
        ArmorUiOwner::screenSpace});
    fixture.CheckFrozenWhitePresentation();
    fixture.CheckSequentialNativeScopes();
    fixture.CheckRejectedFrames();
    fixture.CheckEmptyNativeScopes();
    fixture.CheckNestedScope(ArmorUiOwner::hud, false);
    fixture.CheckNestedScope(ArmorUiOwner::hud, true);
    fixture.CheckNestedScope(ArmorUiOwner::screenSpace, false);
    fixture.CheckNestedScope(ArmorUiOwner::screenSpace, true);
    fixture.CheckUnrelatedScope();
    fixture.CheckFrozenWhitePresentation();
    fixture.CheckArmorSharedScope();
    fixture.CheckArmorPauseScopes();
    fixture.CheckArmorReadConflicts();
    fixture.CheckArmorDrawConflict();
    fixture.CheckArmorCrossPresentRecovery();
    fixture.CheckArmorCommandListRecovery();
    ResetSharedUiResources(true);
    fixture.PrepareScene();
    fixture.DrawHud(false);
    FO4HDR_WARP_CHECK(!SharedUiFrameReady() && !SharedUiStartupSdr());
    FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    fixture.PresentReadable(0.5);
    fixture.CheckSemanticReset();
    fixture.CheckResourceRetirement();
}

} // namespace fo4hdr::tests::gpu
