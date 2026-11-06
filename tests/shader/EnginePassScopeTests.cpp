// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "hooks/HookRecords.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/ReplacementShaders.hpp"
#include "DisplayDiscovery.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "OperatorConstants.hpp"
#include "RendererLifetime.hpp"
#include "ShaderRegistry.hpp"
#include "SharedSdrFrame.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"

#include <array>
#include <initializer_list>
#include <thread>
#include <utility>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using Pass = RenoDxShaderPass;
using Scope = NativeImageSpacePass;

constexpr std::initializer_list<Pass> kUpdatedSet{Pass::toneMapNewA, Pass::toneMapNewB, Pass::lut};

// Stand-ins for the stock shaders the engine creates; `foreign` stays unregistered.
struct StockPassSet
{
    std::vector<ComPtr<ID3D11PixelShader>> stock;
    ComPtr<ID3D11PixelShader> foreign;

    StockPassSet(Device& device, std::initializer_list<Pass> passes)
    {
        ResetShaderRegistry();
        for (const auto pass : passes)
        {
            auto* shader = stock.emplace_back(
                CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs)).Get();
            CreatedShaderIdentity{.descriptor = FindShaderDescriptor(pass)}.Register(S_OK, &shader);
        }
        foreign = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs);
    }
    // The registry keeps raw pointers, so released stand-ins must leave it.
    ~StockPassSet() { ResetShaderRegistry(); }
    StockPassSet(const StockPassSet&) = delete;
    StockPassSet& operator=(const StockPassSet&) = delete;
};

void BindInScope(Device& device, Scope scope, ID3D11PixelShader* shader)
{
    const auto previous = EnterNativeImageSpacePass(scope);
    device.context->PSSetShader(shader, nullptr, 0);
    ExitNativeImageSpacePass(previous);
}

// Production replacements are eligible here, as in the presentation-context suite.
struct EnginePassScopeFixture : SharedPresentationContextFixture
{
    ComPtr<ID3D11PixelShader> temporalShader;
    ComPtr<ID3D11Buffer> nativeConstants;
    Texture lut, history;

    explicit EnginePassScopeFixture(Device& source) : SharedPresentationContextFixture(source)
    {
        BindHud();
        PrepareObservedProductionBindings(device);
        g_presentation.colorDomainGeneration = g_evidence.resourceGeneration.load();
        PublishActiveDisplay(nullptr, 1);
        g_activeSettings.videoWhiteNits = 203;
        nativeConstants = CreateConstantBuffer(*device.device.Get(), OperatorConstants{});
        temporalShader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs);
        lut = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        history = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    }

    ~EnginePassScopeFixture() { ResetShaderRegistry(); }

    void CheckForeign(Pass pass)
    {
        FO4HDR_WARP_CHECK(record->currentPassKnown && record->currentPassForeign);
        FO4HDR_WARP_CHECK(record->currentPass == pass);
    }

    void CheckUnknown() { FO4HDR_WARP_CHECK(!record->currentPassKnown && !record->currentPassForeign); }

    void CheckScopes()
    {
        const StockPassSet set(device, kUpdatedSet);
        auto* foreign = set.foreign.Get();
        for (const auto& [scope, pass] : std::array{std::pair{Scope::toneMapCinematic, Pass::toneMapNewA},
                 std::pair{Scope::toneMapCinematicFade, Pass::toneMapNewB}, std::pair{Scope::lut, Pass::lut}})
        {
            BindInScope(device, scope, foreign);
            CheckForeign(pass);
            FO4HDR_WARP_CHECK(record->currentReplacementActive && record->nativePixelShader.Get() == foreign);
            FO4HDR_WARP_CHECK((g_evidence.foreignShaderMask.load() & RenoDxShaderBit(pass)) != 0);
        }
        device.context->PSSetShader(foreign, nullptr, 0);
        CheckUnknown();

        // Only the first non-null bind in a scope takes its pass; nested scopes restore.
        const auto outer = EnterNativeImageSpacePass(Scope::toneMapCinematic);
        BindInScope(device, Scope::lut, foreign);
        CheckForeign(Pass::lut);
        device.context->PSSetShader(nullptr, nullptr, 0);
        CheckUnknown();
        device.context->PSSetShader(foreign, nullptr, 0);
        CheckForeign(Pass::toneMapNewA);
        device.context->PSSetShader(foreign, nullptr, 0);
        CheckUnknown();
        ExitNativeImageSpacePass(outer);

        // A scope belongs to its thread.
        std::thread([] { (void)EnterNativeImageSpacePass(Scope::lut); }).join();
        device.context->PSSetShader(foreign, nullptr, 0);
        CheckUnknown();

        // A replay without retained state forgets the classification.
        BindInScope(device, Scope::lut, foreign);
        CheckForeign(Pass::lut);
        ComPtr<ID3D11DeviceContext> deferred;
        ComPtr<ID3D11CommandList> commands;
        CheckHresult(device.device->CreateDeferredContext(0, &deferred), "Create pass scope replay context");
        CheckHresult(deferred->FinishCommandList(FALSE, &commands), "Record pass scope replay");
        device.context->ExecuteCommandList(commands.Get(), FALSE);
        CheckUnknown();

        // A stock bind closes its scope, so a later foreign bind there keeps stock identity only.
        const auto stock = EnterNativeImageSpacePass(Scope::lut);
        device.context->PSSetShader(set.stock.back().Get(), nullptr, 0);
        FO4HDR_WARP_CHECK(record->currentPassKnown && !record->currentPassForeign);
        FO4HDR_WARP_CHECK(record->currentPass == Pass::lut);
        device.context->PSSetShader(foreign, nullptr, 0);
        CheckUnknown();
        ExitNativeImageSpacePass(stock);
    }

    void CheckVariants()
    {
        {
            const StockPassSet original(device, {Pass::toneMapA, Pass::toneMapB, Pass::lut});
            BindInScope(device, Scope::toneMapCinematicFade, original.foreign.Get());
            CheckForeign(Pass::toneMapB);
        }
        // Cleared registry slots must not complete the original family; rejections are recorded.
        for (const auto passes : {std::initializer_list{Pass::toneMapB, Pass::lut},
                 std::initializer_list{Pass::toneMapNewA, Pass::lut},
                 std::initializer_list{Pass::toneMapA, Pass::toneMapB, Pass::toneMapNewA,
                     Pass::toneMapNewB, Pass::lut}})
        {
            const StockPassSet set(device, passes);
            g_evidence.foreignScopeRejectMask = 0;
            BindInScope(device, Scope::toneMapCinematicFade, set.foreign.Get());
            CheckUnknown();
            std::uint32_t registered{};
            for (const auto pass : passes) registered |= RenoDxShaderBit(pass);
            FO4HDR_WARP_CHECK(g_evidence.foreignScopeRejectMask.load() ==
                std::uint32_t{1} << static_cast<std::uint8_t>(Scope::toneMapCinematicFade));
            FO4HDR_WARP_CHECK(g_evidence.foreignScopeRegisteredMask.load() == registered);
        }
    }

    void CheckClassInstances()
    {
        ComPtr<ID3D11ClassLinkage> linkage;
        ComPtr<ID3D11PixelShader> linked;
        ComPtr<ID3D11ClassInstance> instance;
        CheckHresult(device.device->CreateClassLinkage(&linkage), "Create scoped shader linkage");
        const auto& code = generated::test::kFullscreenStatePs;
        CheckHresult(device.device->CreatePixelShader(code.data(), code.size(), linkage.Get(), &linked),
            "Create scoped linked shader");
        CheckHresult(linkage->CreateClassInstance("ForeignPixelValue", 0, 0, 0, 0, &instance),
            "Create scoped shader class");
        const StockPassSet set(device, kUpdatedSet);
        // A class-instance bind stays untracked and leaves the scope to the next bind.
        const auto previous = EnterNativeImageSpacePass(Scope::lut);
        auto* value = instance.Get();
        device.context->PSSetShader(linked.Get(), &value, 1);
        CheckUnknown();
        device.context->PSSetShader(set.foreign.Get(), nullptr, 0);
        CheckForeign(Pass::lut);
        ExitNativeImageSpacePass(previous);
    }

    void CompleteOriginalLut()
    {
        CreatedShaderIdentity identity{.descriptor = FindShaderDescriptor(Pass::taa)};
        auto* shader = temporalShader.Get();
        identity.Register(S_OK, &shader);
        const std::array targets{history.rtv.Get(), proxy.rtv.Get()};
        device.context->OMSetRenderTargets(2, targets.data(), nullptr);
        auto* input = lut.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        device.context->PSSetShader(shader, nullptr, 0);
        device.context->Draw(3, 0);
    }

    void CheckOriginalLutExclusion()
    {
        const StockPassSet set(device, kUpdatedSet);
        auto* foreign = set.foreign.Get();
        auto* target = lut.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        BindInScope(device, Scope::lut, foreign);
        auto* constants = nativeConstants.Get();
        device.context->PSSetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants);
        FO4HDR_WARP_CHECK(record->currentReplacementActive);
        g_productionHealthy = false;
        {
            NativeReplacementDrawProbe probe(*record, foreign, nativeConstants.Get());
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(probe.calls == 1);
        }
        CheckForeign(Pass::lut);
        FO4HDR_WARP_CHECK(record->expectedBoundShader == foreign);
        // A foreign LUT is no original SDR producer, so the temporal copy cannot complete it.
        CompleteOriginalLut();
        FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
    }
};

// SDR mode substitutes a scope-identified LUT exactly like the stock one.
void CheckSdrReplacement(Device& device)
{
    SharedPresentationContextFixture fixture(device);
    fixture.BindHud();
    g_activeSettings = DefaultSdrSettings();
    g_activeSettings.vanillaColorGradingStrengthPercent = 50.0;
    g_sdrModeActive = true;
    FO4HDR_WARP_CHECK(BuildSdrReplacementShaders(device.device.Get()));
    {
        const StockPassSet set(device, kUpdatedSet);
        BindInScope(device, Scope::lut, set.foreign.Get());
        ComPtr<ID3D11PixelShader> bound;
        device.context->PSGetShader(&bound, nullptr, nullptr);
        FO4HDR_WARP_CHECK(bound.Get() == g_replacements.lutSdr.Get());
    }
    g_sdrModeActive = false;
}

}  // namespace

void VerifyEnginePassScopes(Device& device)
{
    auto native = CreateNativeContextDevice(device);
    {
        EnginePassScopeFixture fixture(native);
        fixture.CheckScopes();
        fixture.CheckVariants();
        fixture.CheckClassInstances();
    }
    {
        EnginePassScopeFixture fixture(native);
        fixture.CheckOriginalLutExclusion();
    }
    CheckSdrReplacement(native);
}

}  // namespace fo4hdr::tests::gpu
