// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"

#include <memory>

namespace fo4hdr::tests::gpu {
namespace {

// SDR mode with an operator selected in the SDR profile. The renderer does not
// gate selectability; the settings layer does.
struct SdrOperatorFixture : SharedPresentationContextFixture
{
    std::unique_ptr<OperatorLutBuildResult> savedHdrLut{std::move(RetainedCpuLut(OperatorLutTarget::hdr10))};
    std::unique_ptr<OperatorLutBuildResult> savedSdrLut{std::move(RetainedCpuLut(OperatorLutTarget::sdr))};

    SdrOperatorFixture(Device& source, ProductionToneMapperAlgorithm algorithm) :
        SharedPresentationContextFixture(source)
    {
        BindHud();
        PluginConfiguration configuration{};
        configuration.sdrSettings.productionAlgorithm = algorithm;
        InitializeLiveSettings(configuration);
        SetSdrModeActive(true);
    }

    ~SdrOperatorFixture()
    {
        SetSdrModeActive(false);
        ResetShaderRegistry();
        RetainedCpuLut(OperatorLutTarget::hdr10) = std::move(savedHdrLut);
        RetainedCpuLut(OperatorLutTarget::sdr) = std::move(savedSdrLut);
    }

    ID3D11PixelShader* Bind(RenoDxShaderPass pass)
    {
        ResetShaderRegistry();
        auto* shader = hudShader.Get();
        CreatedShaderIdentity{.descriptor = FindShaderDescriptor(pass)}.Register(S_OK, &shader);
        device.context->PSSetShader(shader, nullptr, 0);
        ComPtr<ID3D11PixelShader> bound;
        device.context->PSGetShader(&bound, nullptr, nullptr);
        return bound.Get();
    }

    static void RetainSdrTable()
    {
        auto result = std::make_unique<OperatorLutBuildResult>();
        result->request = OperatorLutKeyFor(g_activeSettings, OperatorLutTarget::sdr);
        result->data = BuildSdrOperatorLut(g_activeSettings.productionAlgorithm);
        RetainedCpuLut(OperatorLutTarget::sdr) = std::move(result);
    }
};

void CheckRetainedOperatorBinding(Device& device)
{
    SdrOperatorFixture fixture(device, ProductionToneMapperAlgorithm::gt7);
    SdrOperatorFixture::RetainSdrTable();
    FO4HDR_WARP_CHECK(BuildSdrReplacementShaders(device.device.Get()));
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 3U && ReplacementSetMatchesConfiguration());
    // The operator replaces the curve even at neutral scene controls.
    FO4HDR_WARP_CHECK(fixture.Bind(RenoDxShaderPass::toneMapA) == g_replacements.toneMapASdr.Get());
    {
        NativeReplacementDrawProbe probe(*fixture.record, g_replacements.toneMapASdr.Get(),
            g_replacements.operatorConstants.Get(), g_replacements.operatorLutSrv.Get());
        device.context->Draw(3, 0);
        FO4HDR_WARP_CHECK(probe.calls == 1);
    }
    // b13 and t7/s7 were borrowed for the draw; the game's empty slots return.
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> lut;
    ComPtr<ID3D11SamplerState> sampler;
    device.context->PSGetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants);
    device.context->PSGetShaderResources(kProductionOperatorLutSlot, 1, &lut);
    device.context->PSGetSamplers(kProductionOperatorSamplerSlot, 1, &sampler);
    FO4HDR_WARP_CHECK(!constants && !lut && !sampler && g_productionHealthy.load());
}

// Drives end-Present commits until the worker's table publishes.
void PublishPendingTable(Device& device)
{
    for (unsigned attempt = 0; attempt < 6000 && g_lutPending; ++attempt)
    {
        CommitLiveSettingsAtEndPresent(device.device.Get(), device.context.Get());
        if (g_lutPending) Sleep(10);
    }
}

void CheckDeferredOperator(Device& device)
{
    SdrOperatorFixture fixture(device, ProductionToneMapperAlgorithm::agX);
    FO4HDR_WARP_CHECK(InitializeOperatorLutWorker());
    auto* hdrMarker = new OperatorLutBuildResult{};
    RetainedCpuLut(OperatorLutTarget::hdr10).reset(hdrMarker);
    const SdrModeInputs inputs{.requested = true};
    FO4HDR_WARP_CHECK(PrepareSdrMode(inputs, device.device.Get(), device.context.Get()));
    // Without a retained table the native curve draws until the build publishes.
    FO4HDR_WARP_CHECK(g_activeSettings.productionAlgorithm == ProductionToneMapperAlgorithm::native);
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 0U && g_replacements.operatorLutSampler);
    FO4HDR_WARP_CHECK(g_lutPending && g_pendingLutRequest.target == OperatorLutTarget::sdr);
    PublishPendingTable(device);
    FO4HDR_WARP_CHECK(!g_lutPending && !g_liveSettingsError);
    FO4HDR_WARP_CHECK(g_activeSettings.productionAlgorithm == ProductionToneMapperAlgorithm::agX);
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 2U && ReplacementSetMatchesConfiguration());
    // The SDR profile keeps its own peak, and the HDR table stays retained.
    FO4HDR_WARP_CHECK(g_activeSettings.displayPeakNits == DefaultSdrSettings().displayPeakNits);
    const auto& retained = RetainedCpuLut(OperatorLutTarget::sdr);
    FO4HDR_WARP_CHECK(retained && retained->data.target == OperatorLutTarget::sdr);
    FO4HDR_WARP_CHECK(RetainedCpuLut(OperatorLutTarget::hdr10).get() == hdrMarker);
}

void CheckProfileSwitchRetiresPendingTable(Device& device)
{
    SdrOperatorFixture fixture(device, ProductionToneMapperAlgorithm::aces2);
    FO4HDR_WARP_CHECK(InitializeOperatorLutWorker());
    FO4HDR_WARP_CHECK(PrepareSdrMode({.requested = true}, device.device.Get(), device.context.Get()));
    FO4HDR_WARP_CHECK(g_lutPending);
    // The pending SDR table belongs to the profile being left.
    SetSdrModeActive(false);
    FO4HDR_WARP_CHECK(!g_lutPending && !g_sdrModeActive.load());
}

}  // namespace

void VerifySdrOperatorPresentation(Device& device)
{
    CheckRetainedOperatorBinding(device);
    CheckDeferredOperator(device);
    CheckProfileSwitchRetiresPendingTable(device);
}

}  // namespace fo4hdr::tests::gpu
