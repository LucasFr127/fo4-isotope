// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include <algorithm>
#include <thread>

namespace fo4hdr::tests::gpu {
namespace {

constexpr std::array kFallbackDomains{LinearColorDomain::linearBt2020UiRelative,
    LinearColorDomain::encodedSrgbBt709UiRelative, LinearColorDomain::vanillaSdrCodes};
using FallbackPixel = std::array<std::uint16_t, 4>;
enum class FallbackCommand { empty, publicRead, offscreenClear, publicClear };

double DecodeFallbackCode(double code, LinearColorDomain domain)
{
    if (domain == LinearColorDomain::linearBt2020UiRelative) return code;
    const auto magnitude = std::abs(code);
    if (domain == LinearColorDomain::vanillaSdrCodes)
        return std::copysign(std::pow(magnitude, 2.2), code);
    return std::copysign(magnitude <= 0.04045 ? magnitude / 12.92 :
        std::pow((magnitude + 0.055) / 1.055, 2.4), code);
}

struct SharedUiFallbackFixture : SharedPresentationContextFixture
{
    using SharedPresentationContextFixture::SharedPresentationContextFixture;

    std::uint32_t PresentFreshImage(bool encoded)
    {
        PrepareScene();
        if (encoded) DrawHud(false);
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        const auto pixel = ReadPresentationPixel(output.texture.Get());
        CompletePresentFrame(true, false);
        return pixel;
    }

    void CheckRepeatedPresent(std::uint32_t expected, bool startup = false)
    {
        const auto productionFrames = g_evidence.productionFrameCount.load();
        const auto generation = g_evidence.resourceGeneration.load();
        if (startup) CheckAssumedPresentation(LinearColorDomain::vanillaSdrCodes);
        else
        {
            const auto retained = SharedUiPresentationForOutput();
            FO4HDR_WARP_CHECK(retained && retained->white == 203);
        }
        constexpr float sentinel[4]{0.8F, 0.3F, 0.6F, 1};
        device.context->ClearRenderTargetView(output.rtv.Get(), sentinel);
        FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
        FO4HDR_WARP_CHECK(ReadPresentationPixel(output.texture.Get()) == expected);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
        FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() == generation);
        FO4HDR_WARP_CHECK(g_evidence.productionFrameCount.load() == productionFrames);
        FO4HDR_WARP_CHECK(!g_evidence.productionActive.load());
        FO4HDR_WARP_CHECK(g_evidence.hdrColorSpaceActive.load());
        CompletePresentFrame(false, false);
    }

    void CheckRepeatedImages()
    {
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        for (const bool encoded : {false, true})
        {
            const auto expected = PresentFreshImage(encoded);
            g_activeSettings.uiPaperWhiteNits = 406;
            CheckRepeatedPresent(expected);
            BindHud();
            {
                const ArmorUiScope empty({.owner = ArmorUiOwner::other,
                    .threadId = GetCurrentThreadId(), .hasRoot = true});
                FO4HDR_WARP_CHECK(empty.Entered());
            }
            device.context->CopyResource(offscreen.texture.Get(), proxy.texture.Get());
            device.context->CopyResource(offscreen.staging.Get(), offscreen.texture.Get());
            const auto retained = SharedUiPresentationForOutput();
            FO4HDR_WARP_CHECK(retained && !retained->assumed);
            CheckRepeatedPresent(expected);
            CheckRepeatedPresent(expected);
        }
    }

    void MutateTexture(const Texture& target, bool copy, bool foreign)
    {
        device.context->CopyResource(target.staging.Get(), target.texture.Get());
        const auto mutate = [&] {
            constexpr float clear[4]{0.25F, 0.25F, 0.25F, 1};
            if (copy) device.context->CopyResource(target.texture.Get(), target.staging.Get());
            else device.context->ClearRenderTargetView(target.rtv.Get(), clear);
        };
        if (foreign)
        {
            std::thread worker(mutate);
            worker.join();
        }
        else mutate();
    }

    void CheckAssumedPresentation(LinearColorDomain domain)
    {
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        FO4HDR_WARP_CHECK(!SharedUiStartupSdr());
        if (domain == LinearColorDomain::vanillaSdrCodes)
        {
            const auto startup = SharedUiStartupPresentation();
            FO4HDR_WARP_CHECK(startup && startup->domain == domain && startup->assumed);
            FO4HDR_WARP_CHECK(startup->white == 203);
            FO4HDR_WARP_CHECK(SharedUiStartupAssumed() && SharedUiStartupPaperWhite() == 203);
            return;
        }
        const auto retained = SharedUiPresentationForOutput();
        FO4HDR_WARP_CHECK(retained && retained->assumed && retained->white == 203);
        FO4HDR_WARP_CHECK(retained->domain == domain);
    }

    void PrepareRepresentation(LinearColorDomain domain)
    {
        if (domain == LinearColorDomain::vanillaSdrCodes) PrepareBlockedImage(true);
        else
        {
            PrepareScene();
            if (domain == LinearColorDomain::encodedSrgbBt709UiRelative)
            {
                BindHud();
                const ArmorUiScope scope({.owner = ArmorUiOwner::hud,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                FO4HDR_WARP_CHECK(scope.Entered());
            }
        }
    }

    void CheckAssumedClearAndCopy()
    {
        for (const bool foreign : {false, true})
        for (const bool copy : {false, true})
        {
            PrepareRepresentation(LinearColorDomain::vanillaSdrCodes);
            MutateTexture(proxy, copy, foreign);
            if (!copy && !foreign)
            {
                FO4HDR_WARP_CHECK(SharedUiStartupSdr() && !SharedUiStartupAssumed());
                PresentReadable(std::pow(0.25, 2.2));
                continue;
            }
            CheckAssumedPresentation(LinearColorDomain::vanillaSdrCodes);
            constexpr float changed[4]{0.25F, 0.25F, 0.25F, 1};
            device.context->ClearRenderTargetView(proxy.rtv.Get(), changed);
            CheckAssumedPresentation(LinearColorDomain::vanillaSdrCodes);
            g_activeSettings.uiPaperWhiteNits = 406;
            PresentReadable(std::pow(0.25, 2.2));
            const auto expected = ReadPresentationPixel(output.texture.Get());
            CheckRepeatedPresent(expected, true);
            CheckRepeatedPresent(expected, true);
            g_activeSettings.uiPaperWhiteNits = 203;
            FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
            FO4HDR_WARP_CHECK(SharedUiStartupSdr() && !SharedUiStartupAssumed());
        }
        CheckFrozenWhitePresentation();
    }

    void CheckPublicUavMutations()
    {
        ReplacePublicImage(CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS));
        for (const auto domain : kFallbackDomains)
        for (const bool foreign : {false, true})
        {
            PrepareRepresentation(domain);
            const auto clear = [&] {
                constexpr float changed[4]{0.25F, 0.25F, 0.25F, 1};
                device.context->ClearUnorderedAccessViewFloat(proxy.uav.Get(), changed);
            };
            if (foreign) std::thread(clear).join();
            else clear();
            CheckAssumedPresentation(domain);
            g_activeSettings.uiPaperWhiteNits = 406;
            PresentReadable(DecodeFallbackCode(0.25, domain));
            const auto expected = ReadPresentationPixel(output.texture.Get());
            CheckRepeatedPresent(expected, domain == LinearColorDomain::vanillaSdrCodes);
            CheckFrozenWhitePresentation();
        }
    }

    void CheckBoundedPixels(const std::array<FallbackPixel, kWidth>& source,
        LinearColorDomain domain, double peak)
    {
        device.context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read bounded assumed output");
        const auto actual = *static_cast<const std::array<std::uint32_t, kWidth>*>(mapped.pData);
        device.context->Unmap(output.staging.Get(), 0);
        const auto ceiling = QuantizeR10Unorm(EncodeSt2084Pq(peak));
        for (UINT x = 0; x < kWidth; ++x)
        {
            LinearRgb nits{DecodeFallbackCode(Binary16ToFloat(source[x][0]), domain) * 203,
                DecodeFallbackCode(Binary16ToFloat(source[x][1]), domain) * 203,
                DecodeFallbackCode(Binary16ToFloat(source[x][2]), domain) * 203};
            if (domain != LinearColorDomain::linearBt2020UiRelative)
                nits = ConvertBt709ToBt2020(nits);
            const std::array channels{nits.red, nits.green, nits.blue};
            for (UINT channel = 0; channel < 3; ++channel)
            {
                const auto value = channels[channel];
                const auto bounded = std::isnan(value) ? 0 : std::clamp(value, 0.0, peak);
                const auto expected = QuantizeR10Unorm(EncodeSt2084Pq(bounded));
                const auto code = static_cast<std::uint16_t>((actual[x] >> (10 * channel)) & 1023);
                FO4HDR_WARP_CHECK(CodeDistance(code, expected) <= 1 && code <= ceiling + 1);
            }
            FO4HDR_WARP_CHECK(actual[x] >> 30 == 3);
        }
    }

    void CheckAssumedPeakContainment()
    {
        constexpr double peak = 600;
        constexpr std::array<FallbackPixel, kWidth> pixels{{
            {0xbc00, 0xbc00, 0xbc00, 0x3c00}, {0x3400, 0x3400, 0x3400, 0x3c00},
            {0x4400, 0x4400, 0x4400, 0x3c00}, {0x4800, 0x0000, 0x0000, 0x3c00},
            {0x7bff, 0x7bff, 0x7bff, 0x3c00}, {0x7c00, 0x7c00, 0x7c00, 0x3c00},
            {0xfc00, 0xfc00, 0xfc00, 0x3c00}, {0x7e00, 0x7e00, 0x7e00, 0x3c00}}};
        const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE);
        device.context->UpdateSubresource(source.texture.Get(), 0, nullptr,
            pixels.data(), kWidth * sizeof(FallbackPixel), 0);
        for (const auto domain : kFallbackDomains)
        for (const bool retired : {false, true})
        {
            PrepareRepresentation(domain);
            device.context->CopyResource(proxy.texture.Get(), source.texture.Get());
            CheckAssumedPresentation(domain);
            if (retired) (void)InvalidateSharedUiPresentation();
            const bool publicDeclared = !retired && domain != LinearColorDomain::vanillaSdrCodes;
            FO4HDR_WARP_CHECK(SharedUiPresentationForOutput().has_value() == publicDeclared);
            g_activeSettings.displayPeakNits = peak;
            g_activeSettings.uiPaperWhiteNits = 406;
            for (UINT repeat = 0; repeat < 2; ++repeat)
            {
                FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
                CheckBoundedPixels(pixels, domain, peak);
                FO4HDR_WARP_CHECK(g_referenceHealthy.load() && !SharedUiFrameReady());
                CompletePresentFrame(false, false);
            }
            FO4HDR_WARP_CHECK(ReadPublicPixels() == pixels);
        }
        g_activeSettings.displayPeakNits = 1000;
        CheckFrozenWhitePresentation();
    }

    void CheckDeclaredPeakUnchanged()
    {
        for (const auto domain : kFallbackDomains)
        {
            PrepareScene();
            constexpr float bright[4]{4, 4, 4, 1};
            record->nextClearRenderTargetView(device.context.Get(), proxy.rtv.Get(), bright);
            PublishSharedUiScene(CaptureSharedUiWrite(true));
            if (domain == LinearColorDomain::encodedSrgbBt709UiRelative)
            {
                BindHud();
                const ArmorUiScope scope({.owner = ArmorUiOwner::hud,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                FO4HDR_WARP_CHECK(scope.Entered());
            }
            if (domain == LinearColorDomain::vanillaSdrCodes)
            {
                PrepareBlockedImage(true);
                device.context->ClearRenderTargetView(proxy.rtv.Get(), bright);
                FO4HDR_WARP_CHECK(SharedUiStartupSdr() && !SharedUiStartupAssumed());
            }
            else
            {
                const auto declared = SharedUiPresentationForOutput();
                FO4HDR_WARP_CHECK(declared && !declared->assumed);
            }
            g_activeSettings.displayPeakNits = 600;
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true) ==
                (domain != LinearColorDomain::vanillaSdrCodes));
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(
                (domain == LinearColorDomain::vanillaSdrCodes ? std::pow(4.0, 2.2) : 4.0) * 203)));
            CompletePresentFrame(domain != LinearColorDomain::vanillaSdrCodes, false);
        }
        g_activeSettings.displayPeakNits = 1000;
        CheckFrozenWhitePresentation();
    }

    void CheckAssumedDescendant()
    {
        PrepareScene();
        MutateTexture(proxy, false, false);
        CheckAssumedPresentation(LinearColorDomain::linearBt2020UiRelative);
        auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto descendant = CreateConstantBuffer(*device.device.Get(),
            HudConstants{.operation = 2});
        for (const auto* target : {&offscreen, &proxy})
        {
            BindHud();
            auto* outputView = target->rtv.Get();
            auto* input = target == &offscreen ? proxy.srv.Get() : offscreen.srv.Get();
            auto* constants = descendant.Get();
            device.context->OMSetRenderTargets(1, &outputView, nullptr);
            device.context->PSSetShaderResources(0, 1, &input);
            device.context->PSSetConstantBuffers(0, 1, &constants);
            record->currentPass = RenoDxShaderPass::fxaa;
            record->currentPassKnown = true;
            record->expectedBoundShader = hudShader.Get();
            device.context->Draw(3, 0);
            input = nullptr;
            device.context->PSSetShaderResources(0, 1, &input);
        }
        FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
        CheckAssumedPresentation(LinearColorDomain::linearBt2020UiRelative);
        PresentReadable(0.25);
        PrepareScene();
        const auto recovered = SharedUiPresentationForOutput();
        FO4HDR_WARP_CHECK(recovered && !recovered->assumed && SharedUiFrameReady());
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(true, false);
    }

    void CheckPublicationOrder()
    {
        for (const bool copy : {false, true})
        {
            PrepareScene();
            const auto beforeMutation = CaptureSharedUiWrite(true);
            FO4HDR_WARP_CHECK(!beforeMutation.assumed);
            MutateTexture(proxy, copy, false);
            PublishSharedUiScene(beforeMutation);
            CheckAssumedPresentation(LinearColorDomain::linearBt2020UiRelative);
            PresentReadable(copy ? 0.5 : 0.25);
        }

        PrepareScene();
        const auto beforeRetirement = CaptureSharedUiWrite(true);
        (void)InvalidateSharedUiPresentation();
        PublishSharedUiScene(beforeRetirement);
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        PresentReadable(0.5);
        PrepareScene();
        constexpr float fresh[4]{0.5F, 0.5F, 0.5F, 1};
        record->nextClearRenderTargetView(device.context.Get(), proxy.rtv.Get(), fresh);
        const auto completed = CaptureSharedUiWrite(true);
        FO4HDR_WARP_CHECK(!completed.assumed && completed.revision != beforeRetirement.revision);
        PublishSharedUiScene(completed);
        const auto recovered = SharedUiPresentationForOutput();
        FO4HDR_WARP_CHECK(recovered && !recovered->assumed && recovered->white == 203);
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(true, false);
    }

    void CheckForeignPublication()
    {
        for (const auto domain : {LinearColorDomain::linearBt2020UiRelative,
                 LinearColorDomain::encodedSrgbBt709UiRelative})
        {
            PrepareRepresentation(domain);
            const auto owner = SharedUiPresentationForOutput();
            FO4HDR_WARP_CHECK(owner && !owner->assumed);
            g_activeSettings.uiPaperWhiteNits = 406;
            SharedUiWriteEvidence foreign;
            std::thread worker([&] {
                foreign = CaptureSharedUiWrite(false);
                PublishSharedUiScene(foreign);
                PublishSharedUiPresentation();
            });
            worker.join();
            FO4HDR_WARP_CHECK(foreign.assumed);
            const auto retained = SharedUiPresentationForOutput();
            FO4HDR_WARP_CHECK(retained && retained->domain == owner->domain);
            FO4HDR_WARP_CHECK(retained->white == owner->white && !retained->assumed);
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            g_activeSettings.uiPaperWhiteNits = 203;
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
            CompletePresentFrame(true, false);
        }
    }

    void CheckAssumedSdrHandoff()
    {
        PrepareRepresentation(LinearColorDomain::vanillaSdrCodes);
        MutateTexture(proxy, true, false);
        CheckAssumedPresentation(LinearColorDomain::vanillaSdrCodes);
        g_configuration.enabled = false;
        FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(0.5));
        FO4HDR_WARP_CHECK(!g_evidence.hdrColorSpaceActive.load() && !g_evidence.productionActive.load());
        CompletePresentFrame(false, false);
        g_configuration.enabled = true;
        CheckFrozenWhitePresentation();
    }

    void CheckStartupInitializationPredication()
    {
        PrepareBlockedImage(true);
        constexpr float bright[4]{1, 1, 1, 1};
        device.context->ClearRenderTargetView(proxy.rtv.Get(), bright);
        const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        ComPtr<ID3D11Predicate> predicate;
        CheckHresult(device.device->CreatePredicate(&description, &predicate),
            "Create startup initialization predicate");
        device.context->Begin(predicate.Get());
        device.context->End(predicate.Get());
        device.context->SetPredication(predicate.Get(), FALSE);
        FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
        ComPtr<ID3D11Predicate> restored;
        BOOL value{TRUE};
        device.context->GetPredication(&restored, &value);
        FO4HDR_WARP_CHECK(restored.Get() == predicate.Get() && value == FALSE);
        FO4HDR_WARP_CHECK(SharedUiStartupSdr() && !SharedUiStartupAssumed());
        device.context->SetPredication(nullptr, FALSE);
        for (const auto& pixel : ReadPublicPixels())
            FO4HDR_WARP_CHECK(pixel == FallbackPixel({0, 0, 0, 0x3c00}));
        PresentReadable(0);
        CheckFrozenWhitePresentation();
    }

    void CheckStartupScaleConfidence()
    {
        for (const bool assumed : {true, false})
        {
            PrepareBlockedImage(true);
            if (assumed) MutateTexture(proxy, true, false);
            FO4HDR_WARP_CHECK(ScaleSharedSdrVideo(device.context.Get(), 100, 300));
            const auto startup = SharedUiStartupPresentation();
            FO4HDR_WARP_CHECK(startup && startup->white == 300 && startup->assumed == assumed);
            FO4HDR_WARP_CHECK(SharedUiStartupSdr() != assumed);
            PresentReadable(std::pow(0.5, 2.2) * 100 / 300, 300);
        }
        CheckFrozenWhitePresentation();
    }

    void CheckHardRetirement()
    {
        PrepareScene();
        MutateTexture(proxy, false, false);
        CheckAssumedPresentation(LinearColorDomain::linearBt2020UiRelative);
        (void)InvalidateSharedUiPresentation();
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        g_activeSettings.uiPaperWhiteNits = 406;
        PresentReadable(0.25);
        CheckFrozenWhitePresentation();

        PrepareScene();
        MutateTexture(proxy, false, false);
        CheckAssumedPresentation(LinearColorDomain::linearBt2020UiRelative);
        ResetSharedUiResources();
        g_scene = {};
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput() && !SharedUiStartupAssumed());
        const auto ui = g_uiTransport;
        g_uiTransport = {};
        g_presentation.presentationProxyRtv.Reset();
        g_presentation.presentationBackBufferRtv.Reset();
        PresentReadable(0.25);
        g_presentation.presentationProxyRtv = proxy.rtv;
        g_uiTransport = ui;
        FO4HDR_WARP_CHECK(EnsureSharedUiResources(device.device.Get()));
        CheckFrozenWhitePresentation();

        PrepareScene();
        BindHud();
        record->currentReplacementActive = true;
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        PresentReadable(0.5);
        CheckFrozenWhitePresentation();
    }

    void ReplacePublicImage(const Texture& replacement)
    {
        RetireSharedUiPublicImage();
        proxy = replacement;
        g_presentation.presentationProxy = proxy.texture;
        g_presentation.presentationProxyRtv = proxy.rtv;
        g_presentation.presentationProxySrv = proxy.srv;
        g_presentation.presentationProxyUav = proxy.uav;
    }

    void CheckEmergencyLifetime()
    {
        PrepareScene();
        const auto stale = CaptureSharedUiWrite(true);
        const auto original = proxy;
        ReplacePublicImage(CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        PublishSharedUiPublicImage({LinearColorDomain::linearBt2020UiRelative, 203}, stale);
        FO4HDR_WARP_CHECK(!SharedUiEmergencyPresentation());
        PresentBlack();
        ReplacePublicImage(original);
        PublishSharedUiPublicImage({LinearColorDomain::linearBt2020UiRelative, 203}, stale);
        FO4HDR_WARP_CHECK(!SharedUiEmergencyPresentation());
        PresentBlack();
        PrepareScene();
        const auto beforeClear = CaptureSharedUiWrite(true);
        g_scene = {};
        g_activeSettings.uiPaperWhiteNits = 300;
        FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
        PublishSharedUiPublicImage({LinearColorDomain::linearBt2020UiRelative, 203}, beforeClear);
        const auto cleared = SharedUiEmergencyPresentation();
        FO4HDR_WARP_CHECK(cleared && cleared->domain == LinearColorDomain::vanillaSdrCodes);
        FO4HDR_WARP_CHECK(cleared->white == 300 && cleared->assumed);
        PresentReadable(0, 300);
        CheckFrozenWhitePresentation();
    }

    void CheckEmergencyLifecycle()
    {
        PrepareScene();
        const auto producer = CaptureSharedUiWrite(true);
        g_activeSettings.uiPaperWhiteNits = 406;
        PublishGraphicsLifecycle(false, true);
        FO4HDR_WARP_CHECK(!PreparePresentFrame(chain.Get(), true));
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)));
        CompletePresentFrame(false, false);
        PublishSharedUiScene(producer);
        FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput());
        g_currentFrameQualifiedDrawThreadConflict = true;
        PresentReadable(0.5);
        FO4HDR_WARP_CHECK(!g_liveSettingsThreadCompatible.load());
        PrepareObservedProductionBindings(device);
        PublishGraphicsLifecycle(true, false);
        ApplyGraphicsLifecycleBeforeDraw(device.context.Get());
        g_liveSettingsThreadCompatible = true;
        CheckFrozenWhitePresentation();
    }

    void CheckForeignRequiredShaders()
    {
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        for (const bool publicTarget : {false, true})
        {
            g_productionHealthy = true;
            PrepareScene();
            DrawHud(false);
            BindHud();
            auto* target = publicTarget ? proxy.rtv.Get() : offscreen.rtv.Get();
            device.context->OMSetRenderTargets(1, &target, nullptr);
            const auto foreign = CreateConstantBuffer(*device.device.Get(),
                HudConstants{.color = {0.25F, 0.25F, 0.25F, 1}});
            auto* constants = foreign.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
            record->currentPass = RenoDxShaderPass::toneMapA;
            record->currentPassKnown = true;
            record->currentReplacementActive = true;
            record->currentRequirement = PipelinePassRequirement::required;
            record->expectedBoundShader = g_presentation.presentShader.Get();
            struct DrawProbe { ID3D11DeviceContext* context; UINT calls{}; } probe{device.context.Get()};
            ExecuteObservedDraw(*record, device.context.Get(), {[](void* arguments) {
                auto& invocation = *static_cast<DrawProbe*>(arguments);
                ++invocation.calls;
                FindContext(invocation.context)->nextDraw(invocation.context, 3, 0);
            }, &probe});
            FO4HDR_WARP_CHECK(probe.calls == 1 && !g_productionHealthy.load() && g_referenceHealthy.load());
            PresentReadable(publicTarget ? DecodeFallbackCode(0.25,
                LinearColorDomain::encodedSrgbBt709UiRelative) : 0.5);
            const auto expected = ReadPresentationPixel(output.texture.Get());
            CheckRepeatedPresent(expected);
            CheckRepeatedPresent(expected);
            ComPtr<ID3D11PixelShader> actual;
            device.context->PSGetShader(&actual, nullptr, nullptr);
            FO4HDR_WARP_CHECK(actual.Get() == hudShader.Get() && !g_productionHealthy.load());
        }
    }

    void CheckFlatUiSourceColor()
    {
        const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        constexpr float sample[4]{0.75F, 0.25F, 0.125F, 1};
        device.context->ClearRenderTargetView(source.rtv.Get(), sample);
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
        CheckHresult(device.device->CreateBlendState(&description, &blend), "Create FlatUI arithmetic blend");
        for (const float alpha : {1.0F, 0.5F})
        {
            PrepareScene();
            BindHud();
            const auto tint = CreateConstantBuffer(*device.device.Get(),
                HudConstants{.operation = 5, .color = {0.5F, 0.75F, 1, alpha}});
            auto* constants = tint.Get();
            auto* input = source.srv.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
            device.context->PSSetShaderResources(0, 1, &input);
            {
                // Arithmetic fixture for sample*tint; native game shader parity is separate.
                const ArmorUiScope scope({.owner = ArmorUiOwner::flatMenu,
                    .threadId = GetCurrentThreadId(), .postAa = true, .hasRoot = true});
                FO4HDR_WARP_CHECK(scope.Entered() && SharedUiFrameEncoded());
                device.context->OMSetBlendState(blend.Get(), nullptr, ~0U);
                device.context->Draw(3, 0);
            }
            input = nullptr;
            device.context->PSSetShaderResources(0, 1, &input);
            const auto pixels = ReadPublicPixels();
            constexpr std::array product{0.375, 0.1875, 0.125};
            const auto behind = 1.055 * std::pow(0.5, 1.0 / 2.4) - 0.055;
            for (UINT channel = 0; channel < 3; ++channel)
                FO4HDR_WARP_CHECK(std::abs(Binary16ToFloat(pixels[0][channel]) -
                    (product[channel] * alpha + behind * (1 - alpha))) < 0.001);
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            CheckBoundedPixels(pixels, LinearColorDomain::encodedSrgbBt709UiRelative, 1000);
            CompletePresentFrame(true, false);
        }
    }

    void CheckPublicImageMutations()
    {
        for (const bool encoded : {false, true})
        {
            const auto previous = PresentFreshImage(encoded);
            BindHud();
            const auto native = CreateConstantBuffer(*device.device.Get(),
                HudConstants{.color = {0.25F, 0.25F, 0.25F, 1}});
            auto* constants = native.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(SharedUiPresentationForOutput());
            const auto linear = encoded ? std::pow((0.25 + 0.055) / 1.055, 2.4) : 0.25;
            PresentReadable(linear);
            FO4HDR_WARP_CHECK(ReadPresentationPixel(output.texture.Get()) != previous);
            CheckFrozenWhitePresentation();
            for (const bool foreign : {false, true})
            for (const bool copy : {false, true})
            {
                (void)PresentFreshImage(encoded);
                MutateTexture(proxy, copy, foreign);
                CheckAssumedPresentation(encoded ? LinearColorDomain::encodedSrgbBt709UiRelative :
                    LinearColorDomain::linearBt2020UiRelative);
                g_activeSettings.uiPaperWhiteNits = 406;
                PresentReadable(copy ? 0.5 : linear);
                const auto expected = ReadPresentationPixel(output.texture.Get());
                CheckRepeatedPresent(expected);
                CheckRepeatedPresent(expected);
                CheckFrozenWhitePresentation();
                const auto recovered = SharedUiPresentationForOutput();
                FO4HDR_WARP_CHECK(recovered && !recovered->assumed);
            }
        }
    }

    void CheckForeignSceneMutations()
    {
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        constexpr float scene[4]{0.25F, 0.25F, 0.25F, 1};
        device.context->ClearRenderTargetView(offscreen.rtv.Get(), scene);
        for (const bool copy : {false, true})
        {
            PrepareScene();
            MutateTexture(offscreen, copy, true);
            FO4HDR_WARP_CHECK(SharedUiFrameReady());
            FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
            CompletePresentFrame(true, false);
            for (const bool selectedOffscreen : {false, true})
            {
                PrepareScene();
                if (selectedOffscreen) g_scene.currentScene = offscreen.srv;
                PublishSharedUiScene();
                MutateTexture(selectedOffscreen ? offscreen : proxy, copy, true);
                FO4HDR_WARP_CHECK(!SharedUiFrameReady());
                DrawHud(false);
                FO4HDR_WARP_CHECK(!SharedUiFrameReady());
                PresentReadable(selectedOffscreen || copy ? 0.5 : 0.25);
                CheckFrozenWhitePresentation();
            }
        }
    }

    void CheckFirstActivationFallback()
    {
        FO4HDR_WARP_CHECK(g_lifecycle.DeactivateToSdr());
        g_evidence.resourceGeneration = g_lifecycle.Snapshot().resourceGeneration;
        const auto generation = g_evidence.resourceGeneration.load();
        const auto expected = PresentFreshImage(true);
        FO4HDR_WARP_CHECK(g_evidence.resourceGeneration.load() > generation);
        g_activeSettings.uiPaperWhiteNits = 406;
        CheckRepeatedPresent(expected);
    }

    void CheckNativeUiBeforeSeed()
    {
        for (const bool backdrop : {false, true})
        {
            PrepareScene();
            BindHud();
            auto shader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
            CreatedShaderIdentity identity;
            identity.lateGameUiComposite = !backdrop;
            identity.lateGameUiBackdrop = backdrop;
            auto* raw = shader.Get();
            identity.Register(S_OK, &raw);
            const auto mask = CreateTexture(*device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D11_BIND_SHADER_RESOURCE);
            const auto native = CreateConstantBuffer(*device.device.Get(),
                HudConstants{.color = {0.25F, 0.25F, 0.25F, 1}});
            auto* input = mask.srv.Get();
            auto* constants = native.Get();
            device.context->PSSetShaderResources(0, 1, &input);
            device.context->PSSetConstantBuffers(0, 1, &constants);
            device.context->PSSetShader(shader.Get(), nullptr, 0);
            ExecuteObservedDraw(*record, device.context.Get(),
                {[](void* context) { static_cast<ID3D11DeviceContext*>(context)->Draw(3, 0); },
                    device.context.Get()});
            const auto value = Binary16ToFloat(ReadPublicPixels()[0][0]);
            FO4HDR_WARP_CHECK(value == 0.25F);
            ResetShaderRegistry();
            BindHud();
            const auto foreign = CreateConstantBuffer(*device.device.Get(),
                HudConstants{.color = {0.75F, 0.75F, 0.75F, 1}, .rectangle = {1, 0, kWidth, kHeight}});
            constants = foreign.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
            {
                const ArmorUiScope scope({.owner = ArmorUiOwner::screenSpace,
                    .threadId = GetCurrentThreadId()});
                FO4HDR_WARP_CHECK(scope.Entered());
                device.context->Draw(3, 0);
            }
            FO4HDR_WARP_CHECK(!SharedUiFrameEncoded() && !SharedUiFrameReady());
            PresentReadable(0.25);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.75 * 203)), 1);
            const auto expected = ReadPresentationPixel(output.texture.Get());
            g_activeSettings.uiPaperWhiteNits = 406;
            CheckRepeatedPresent(expected);
            CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.75 * 203)), 1);
            input = nullptr;
            device.context->PSSetShaderResources(0, 1, &input);
            CheckFrozenWhitePresentation();
        }
    }

    void CheckMutatedSceneDescendant()
    {
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET);
        constexpr float changed[4]{0.25F, 0.25F, 0.25F, 1};
        device.context->ClearRenderTargetView(source.rtv.Get(), changed);
        const auto descendant = CreateConstantBuffer(*device.device.Get(), HudConstants{.operation = 2});
        for (const bool copy : {false, true})
        {
            PrepareScene();
            constexpr float initial[4]{0.5F, 0.5F, 0.5F, 1};
            device.context->ClearRenderTargetView(offscreen.rtv.Get(), initial);
            g_scene.currentScene = offscreen.srv;
            PublishSharedUiScene();
            std::thread worker([&] {
                if (copy) device.context->CopyResource(offscreen.texture.Get(), source.texture.Get());
                else device.context->ClearRenderTargetView(offscreen.rtv.Get(), changed);
            });
            worker.join();
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            BindHud();
            auto* input = offscreen.srv.Get();
            auto* constants = descendant.Get();
            device.context->PSSetShaderResources(0, 1, &input);
            device.context->PSSetConstantBuffers(0, 1, &constants);
            record->currentPass = RenoDxShaderPass::fxaa;
            record->currentPassKnown = true;
            record->expectedBoundShader = hudShader.Get();
            device.context->Draw(3, 0);
            input = nullptr;
            device.context->PSSetShaderResources(0, 1, &input);
            FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            FO4HDR_WARP_CHECK(SharedUiPresentationForOutput());
            PresentReadable(0.25);
        }
        CheckFrozenWhitePresentation();
    }

    void CheckForeignDrawRecovery()
    {
        for (const bool encoded : {false, true})
        {
            PrepareScene();
            if (encoded) DrawHud(false);
            BindHud();
            const auto native = CreateConstantBuffer(*device.device.Get(),
                HudConstants{.color = {0.25F, 0.25F, 0.25F, 1}});
            auto* constants = native.Get();
            device.context->PSSetConstantBuffers(0, 1, &constants);
            std::thread worker([&] { device.context->Draw(3, 0); });
            worker.join();
            FO4HDR_WARP_CHECK(!SharedUiFrameReady());
            FO4HDR_WARP_CHECK(SharedUiPresentationForOutput());
            PresentReadable(encoded ? std::pow((0.25 + 0.055) / 1.055, 2.4) : 0.25);
            CheckFrozenWhitePresentation();
        }
    }

    // An application occlusion scope left open across Present blocks optional
    // injection only; final output still renders and the scope may count it.
    void CheckQueryOpenAcrossPresent(bool startup)
    {
        const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION, 0};
        ComPtr<ID3D11Query> query;
        CheckHresult(device.device->CreateQuery(&description, &query), "Create Present-spanning query");
        PrepareBlockedImage(startup);
        device.context->Begin(query.Get());
        FO4HDR_WARP_CHECK(!record->computeQueries.CanDispatch());
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true) == !startup);
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(
            (startup ? std::pow(0.5, 2.2) : 0.5) * 203)));
        if (startup) FO4HDR_WARP_CHECK(SharedUiStartupSdr());
        CompletePresentFrame(!startup, false);
        device.context->End(query.Get());
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        CheckFrozenWhitePresentation();
    }

    void PrepareBlockedImage(bool startup)
    {
        if (!startup)
        {
            PrepareScene();
            DrawHud(false);
            return;
        }
        g_scene = {};
        g_activeSettings.uiPaperWhiteNits = 203;
        FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
        FO4HDR_WARP_CHECK(SharedUiStartupSdr());
        constexpr float scene[4]{0.5F, 0.5F, 0.5F, 1};
        device.context->ClearRenderTargetView(proxy.rtv.Get(), scene);
    }

    void CheckBlockedStreamOutput(bool startup)
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = 64;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        ComPtr<ID3D11Buffer> buffer;
        CheckHresult(device.device->CreateBuffer(&description, nullptr, &buffer),
            "Create blocked Present stream output");
        PrepareBlockedImage(startup);
        auto* target = buffer.Get();
        constexpr UINT offset = 0;
        device.context->SOSetTargets(1, &target, &offset);
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true) == !startup);
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(
            (startup ? std::pow(0.5, 2.2) : 0.5) * 203)));
        if (startup) FO4HDR_WARP_CHECK(SharedUiStartupSdr());
        ComPtr<ID3D11Buffer> actual;
        device.context->SOGetTargets(1, &actual);
        FO4HDR_WARP_CHECK(actual.Get() == buffer.Get());
        device.context->SOSetTargets(0, nullptr, nullptr);
        CompletePresentFrame(!startup, false);
        CheckFrozenWhitePresentation();
    }

    void CheckLaterSceneDraws()
    {
        PrepareScene();
        DrawHud(false);
        PrepareObservedProductionBindings(device);
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        BindHud();
        auto* target = offscreen.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        record->currentPass = RenoDxShaderPass::lut;
        record->currentPassKnown = true;
        record->currentReplacementActive = true;
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(SharedUiFrameReady() && SharedUiFrameEncoded());
        FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
        FO4HDR_WARP_CHECK(g_scene.temporalPassObserved && g_scene.temporalSceneReady);
        PresentEncodedUi(0.5);

        PrepareScene();
        DrawHud(false);
        BindHud();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        auto* input = proxy.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        record->currentPass = RenoDxShaderPass::fxaa;
        record->currentPassKnown = true;
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        PresentReadable(0.5);
        input = nullptr;
        device.context->PSSetShaderResources(0, 1, &input);
    }

    void CheckKnownReadSlots()
    {
        struct InputCase { RenoDxShaderPass pass; UINT slot; bool rejected; };
        constexpr std::array cases{
            InputCase{RenoDxShaderPass::toneMapA, 0, true},
            InputCase{RenoDxShaderPass::taa2, 4, true},
            InputCase{RenoDxShaderPass::dofAuxiliary3, 2, true},
            InputCase{RenoDxShaderPass::dofAuxiliary3, 1, false},
            InputCase{RenoDxShaderPass::fxaa, 6, false}};
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ComPtr<ID3D11ShaderResourceView> alias;
        CheckHresult(device.device->CreateShaderResourceView(proxy.texture.Get(), nullptr, &alias),
            "Create shared scene read alias");
        for (const auto& test : cases)
        {
            PrepareScene();
            DrawHud(false);
            BindHud();
            auto* target = offscreen.rtv.Get();
            device.context->OMSetRenderTargets(1, &target, nullptr);
            auto* input = alias.Get();
            device.context->PSSetShaderResources(test.slot, 1, &input);
            record->currentPass = test.pass;
            record->currentPassKnown = true;
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(SharedUiFrameReady() != test.rejected);
            input = nullptr;
            device.context->PSSetShaderResources(test.slot, 1, &input);
            if (test.rejected) PresentReadable(0.5);
            else PresentEncodedUi(0.5);
        }
    }

    void CheckMutationRejectionAndRecovery(double linear)
    {
        DrawHud(false);
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        CheckAssumedPresentation(LinearColorDomain::linearBt2020UiRelative);
        PresentReadable(linear);
        CheckFrozenWhitePresentation();
    }

    void CheckPreSeedMutations()
    {
        PrepareScene();
        device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
        device.context->CopyResource(proxy.texture.Get(), proxy.staging.Get());
        CheckMutationRejectionAndRecovery(0.5);
        PrepareScene();
        constexpr float white[4]{1, 1, 1, 1};
        device.context->ClearRenderTargetView(proxy.rtv.Get(), white);
        CheckMutationRejectionAndRecovery(1);
        PrepareScene();
        BindHud();
        device.context->Draw(3, 0);
        DrawHud();
        FO4HDR_WARP_CHECK(!SharedUiFrameReady());
        PresentReadable(0.5);
        CheckFrozenWhitePresentation();
    }

    void CheckCommandListMutation(FallbackCommand contents)
    {
        ComPtr<ID3D11DeviceContext> deferred;
        CheckHresult(device.device->CreateDeferredContext(0, &deferred),
            "Create public-image command-list context");
        const auto offscreen = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        constexpr float changed[4]{0.25F, 0.25F, 0.25F, 1};
        if (contents == FallbackCommand::publicRead)
            deferred->CopyResource(offscreen.texture.Get(), proxy.texture.Get());
        if (contents == FallbackCommand::offscreenClear)
            deferred->ClearRenderTargetView(offscreen.rtv.Get(), changed);
        if (contents == FallbackCommand::publicClear)
            deferred->ClearRenderTargetView(proxy.rtv.Get(), changed);
        ComPtr<ID3D11CommandList> commands;
        CheckHresult(deferred->FinishCommandList(FALSE, &commands),
            "Finish public-image mutation command list");
        for (const bool restore : {false, true})
        for (const auto domain : kFallbackDomains)
        {
            PrepareRepresentation(domain);
            device.context->ExecuteCommandList(commands.Get(), restore);
            CheckAssumedPresentation(domain);
            const auto linear = contents == FallbackCommand::publicClear ?
                DecodeFallbackCode(0.25, domain) :
                (domain == LinearColorDomain::vanillaSdrCodes ? std::pow(0.5, 2.2) : 0.5);
            g_activeSettings.uiPaperWhiteNits = 406;
            PresentReadable(linear);
            const auto expected = ReadPresentationPixel(output.texture.Get());
            CheckRepeatedPresent(expected, domain == LinearColorDomain::vanillaSdrCodes);
            CheckFrozenWhitePresentation();
        }
    }
};

} // namespace

void VerifySharedUiFallback(Device& device)
{
    auto native = CreateNativeContextDevice(device);
    SharedUiFallbackFixture fixture(native);
    FO4HDR_WARP_CHECK(!SharedUiPresentationForOutput() && !SharedUiEmergencyPresentation());
    fixture.PresentBlack();
    fixture.CheckFirstActivationFallback();
    fixture.CheckNativeUiBeforeSeed();
    fixture.CheckRepeatedImages();
    fixture.CheckPublicImageMutations();
    fixture.CheckAssumedClearAndCopy();
    fixture.CheckForeignSceneMutations();
    fixture.CheckMutatedSceneDescendant();
    fixture.CheckForeignDrawRecovery();
    fixture.CheckLaterSceneDraws();
    fixture.CheckKnownReadSlots();
    fixture.CheckPreSeedMutations();
    for (const auto contents : {FallbackCommand::empty, FallbackCommand::publicRead,
             FallbackCommand::offscreenClear, FallbackCommand::publicClear})
        fixture.CheckCommandListMutation(contents);
    fixture.CheckDeclaredPeakUnchanged();
    fixture.CheckAssumedPeakContainment();
    fixture.CheckAssumedDescendant();
    fixture.CheckPublicationOrder();
    fixture.CheckForeignPublication();
    fixture.CheckAssumedSdrHandoff();
    fixture.CheckStartupInitializationPredication();
    fixture.CheckStartupScaleConfidence();
    fixture.CheckHardRetirement();
    fixture.CheckEmergencyLifetime();
    fixture.CheckEmergencyLifecycle();
    fixture.CheckFlatUiSourceColor();
    fixture.CheckPublicUavMutations();
    for (const bool startup : {false, true})
    {
        fixture.CheckQueryOpenAcrossPresent(startup);
        fixture.CheckBlockedStreamOutput(startup);
    }
    fixture.CheckForeignRequiredShaders();
}

} // namespace fo4hdr::tests::gpu
