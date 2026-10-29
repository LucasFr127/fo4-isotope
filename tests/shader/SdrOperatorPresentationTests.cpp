// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"

#include <algorithm>
#include <array>
#include <memory>

namespace fo4hdr::tests::gpu {
namespace {

// ID3D11Device declares CreateTexture3D between CreateTexture2D and CreateShaderResourceView.
constexpr std::size_t kDeviceCreateTexture3D{kDeviceCreateTexture2D + 1U};
static_assert(kDeviceCreateTexture3D + 1U == kDeviceCreateSrv);

// Fails CreateTexture3D on one device while alive. Like a production shadow,
// the object points at a copy of its table that keeps the original RTTI header.
class FailingTexture3DCreation
{
public:
    explicit FailingTexture3DCreation(ID3D11Device* device) noexcept :
        object_(reinterpret_cast<void***>(device)), original_(*object_)
    {
        std::copy_n(original_ - kShadowHeaderSlots, table_.size(), table_.begin());
        table_[kShadowHeaderSlots + kDeviceCreateTexture3D] = reinterpret_cast<void*>(&Fail);
        *object_ = table_.data() + kShadowHeaderSlots;
    }
    ~FailingTexture3DCreation() { *object_ = original_; }
    FailingTexture3DCreation(const FailingTexture3DCreation&) = delete;
    FailingTexture3DCreation& operator=(const FailingTexture3DCreation&) = delete;

private:
    static HRESULT STDMETHODCALLTYPE Fail(ID3D11Device*, const D3D11_TEXTURE3D_DESC*,
        const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D** texture) noexcept
    {
        if (texture != nullptr) *texture = nullptr;
        return E_OUTOFMEMORY;
    }

    void*** object_;
    void** original_;
    std::array<void*, kShadowHeaderSlots + kDeviceBaseSlots> table_{};
};

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

    // Commits an SDR draft through the path end Present takes on a commit request.
    void CommitSdrDraft(const HdrSettings& draft)
    {
        InitializeSettingsWindow({HdrSettings{}, draft}, {});
        SelectSettingsWindowProfile(true);
        CommitUiDraft(device.device.Get(), device.context.Get());
    }
};

void CheckRetainedOperatorBinding(Device& device)
{
    SdrOperatorFixture fixture(device, ProductionToneMapperAlgorithm::gt7);
    SdrOperatorFixture::RetainSdrTable();
    FO4HDR_WARP_CHECK(BuildSdrReplacementShaders(device.device.Get()));
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 3U && ReplacementSetMatchesConfiguration());
    // The shaper lanes follow the table's SDR white; GT7's would differ at the profile's peak.
    const auto& published = g_replacements.configurationKey;
    const auto shaper = OperatorLutShaperParameters(ProductionToneMapperAlgorithm::gt7,
        SdrOperatorWhiteNits(ProductionToneMapperAlgorithm::gt7));
    const std::array shaperParameters{shaper[2], shaper[3]};
    FO4HDR_WARP_CHECK(published.operatorShaperAngleGain == shaper[0] &&
        published.operatorShaperBias == shaper[1] &&
        published.operatorShaperParameters == shaperParameters);
    FO4HDR_WARP_CHECK(OperatorLutShaperParameters(ProductionToneMapperAlgorithm::gt7,
        g_activeSettings.displayPeakNits) != shaper);
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

// The native curve leaves the last SDR table retained; returning to its
// operator publishes that table at once instead of rebuilding it.
void CheckRetainedTableAfterNativeCurve(Device& device)
{
    SdrOperatorFixture fixture(device, ProductionToneMapperAlgorithm::gt7);
    SdrOperatorFixture::RetainSdrTable();
    FO4HDR_WARP_CHECK(BuildSdrReplacementShaders(device.device.Get()));
    const auto* retained = RetainedCpuLut(OperatorLutTarget::sdr).get();
    const auto operatorDraft = g_activeSettings;
    auto nativeDraft = operatorDraft;
    nativeDraft.productionAlgorithm = ProductionToneMapperAlgorithm::native;
    fixture.CommitSdrDraft(nativeDraft);
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 0U && !g_replacements.operatorLutSrv);
    fixture.CommitSdrDraft(operatorDraft);
    FO4HDR_WARP_CHECK(!g_lutPending && !g_liveSettingsError);
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 3U && ReplacementSetMatchesConfiguration());
    FO4HDR_WARP_CHECK(RetainedCpuLut(OperatorLutTarget::sdr).get() == retained);
}

// A retained table that fails to upload keeps SDR mode on the native curve
// until the rebuilt table publishes.
void CheckFailedTableUpload(Device& device)
{
    SdrOperatorFixture fixture(device, ProductionToneMapperAlgorithm::agX);
    FO4HDR_WARP_CHECK(InitializeOperatorLutWorker());
    SdrOperatorFixture::RetainSdrTable();
    {
        const FailingTexture3DCreation failure(device.device.Get());
        FO4HDR_WARP_CHECK(PrepareSdrMode({.requested = true},
            device.device.Get(), device.context.Get()));
    }
    FO4HDR_WARP_CHECK(g_sdrModeActive.load() && g_replacements.configurationKey.mode == 0U);
    FO4HDR_WARP_CHECK(g_activeSettings.productionAlgorithm == ProductionToneMapperAlgorithm::native);
    FO4HDR_WARP_CHECK(g_lutPending && g_pendingLutRequest.target == OperatorLutTarget::sdr);
    PublishPendingTable(device);
    FO4HDR_WARP_CHECK(!g_lutPending && !g_liveSettingsError);
    FO4HDR_WARP_CHECK(g_activeSettings.productionAlgorithm == ProductionToneMapperAlgorithm::agX);
    FO4HDR_WARP_CHECK(g_replacements.configurationKey.mode == 2U && ReplacementSetMatchesConfiguration());
}

}  // namespace

void VerifySdrOperatorPresentation(Device& device)
{
    CheckRetainedOperatorBinding(device);
    CheckDeferredOperator(device);
    CheckProfileSwitchRetiresPendingTable(device);
    CheckRetainedTableAfterNativeCurve(device);
    CheckFailedTableUpload(device);
}

}  // namespace fo4hdr::tests::gpu
