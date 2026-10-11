// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "SharedSdrFrame.hpp"
#include "DrawObservation.hpp"
#include "FrameTrace.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "RendererLifetime.hpp"
#include "ShaderRegistry.hpp"
#include "SharedUiFrame.hpp"
#include "hooks/ContextHooks.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <tuple>
#include <utility>
#include <wrl/client.h>

namespace fo4hdr {
namespace {
using Microsoft::WRL::ComPtr;

enum class SdrRejection : std::uint32_t
{
    none, context, replacement, mutation, producer, completion, incomplete, videoScale
};

struct SdrFrameIdentity
{
    std::uint64_t generation{}, contextGeneration{}, frame{}, resetEpoch{};
    std::uintptr_t context{}, resource{};
    DWORD thread{};
    bool operator==(const SdrFrameIdentity&) const = default;
};

struct SdrFrameContent
{
    SdrFrameIdentity identity;
    ComPtr<ID3D11Resource> originalLut;
    std::uint64_t revision{};
    float white{}, videoWhite{};
    bool produced{}, videoProduced{}, publicDrawn{}, complete{};
};

SdrFrameContent g_content;
std::atomic<bool> g_armed{}, g_rejected{};
std::atomic<DWORD> g_ownerThread{};
// Foreign mutations invalidate only this token; the owner retains the COM reference.
std::atomic<std::uintptr_t> g_originalLutIdentity{};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
std::atomic<std::uint64_t> g_lastWriterFrame{std::numeric_limits<std::uint64_t>::max()};
std::atomic<std::uint64_t> g_lastWriterGeneration{}, g_lastWriterContextGeneration{};
std::atomic<std::uint64_t> g_arms{}, g_presented{}, g_rejectedPresents{};
std::atomic<SdrRejection> g_frameRejection{}, g_lastRejection{};

void Reject(SdrRejection reason) noexcept
{
    auto expected = SdrRejection::none;
    if (g_frameRejection.compare_exchange_strong(expected, reason, std::memory_order_acq_rel))
        g_lastRejection.store(reason, std::memory_order_release);
    g_rejected.store(true, std::memory_order_release);
}

SdrFrameIdentity CurrentIdentity(ID3D11DeviceContext* context) noexcept
{
    const auto* record = FindDrawObservation(context);
    return {g_evidence.resourceGeneration.load(std::memory_order_acquire),
        record ? record->generation : 0, g_frame, ReadGraphicsLifecycle().resetEpoch,
        reinterpret_cast<std::uintptr_t>(context),
        reinterpret_cast<std::uintptr_t>(g_presentation.presentationProxy.Get()),
        GetCurrentThreadId()};
}

bool HasCurrentWriter(const SdrFrameIdentity& identity) noexcept
{
    return g_lastWriterFrame.load(std::memory_order_acquire) == identity.frame &&
        g_lastWriterGeneration.load(std::memory_order_acquire) == identity.generation &&
        g_lastWriterContextGeneration.load(std::memory_order_acquire) == identity.contextGeneration;
}

void MarkCurrentWriter(ID3D11DeviceContext* context) noexcept
{
    const auto identity = CurrentIdentity(context);
    g_lastWriterGeneration.store(identity.generation, std::memory_order_release);
    g_lastWriterContextGeneration.store(identity.contextGeneration, std::memory_order_release);
    g_lastWriterFrame.store(identity.frame, std::memory_order_release);
}

bool SdrPathRequested() noexcept
{
    const std::array ready{HdrPathRequested(), g_configuration.productionRequested,
        g_referenceHealthy.load(std::memory_order_acquire), ApplicationPresentationFacadeActive(),
        !g_gameplayReadyApplied.load(std::memory_order_acquire) || !g_productionHealthy.load()};
    return std::ranges::all_of(ready, [](bool value) { return value; });
}

bool ContextReady(ID3D11DeviceContext* context) noexcept
{
    if (!context || context != g_rendererDevice.activeContext)
        return false;
    const auto* record = FindDrawObservation(context);
    return record && record->immediate &&
        g_liveSettingsThreadCompatible.load(std::memory_order_acquire);
}

bool CurrentEpoch(ID3D11DeviceContext* context) noexcept
{
    if (!g_armed.load(std::memory_order_acquire))
        return false;
    if (g_ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId())
        return false;
    return ContextReady(context) && g_content.identity == CurrentIdentity(context);
}

ComPtr<ID3D11Resource> ViewResource(ID3D11View* view) noexcept
{
    ComPtr<ID3D11Resource> resource;
    if (view)
        view->GetResource(&resource);
    return resource;
}

void SetOriginalLut(ComPtr<ID3D11Resource> resource) noexcept
{
    g_originalLutIdentity.store(0, std::memory_order_release);
    g_content.originalLut = std::move(resource);
    g_originalLutIdentity.store(CanonicalResourceIdentity(g_content.originalLut.Get()),
        std::memory_order_release);
}

ComPtr<ID3D11Resource> OriginalLutResource(ID3D11RenderTargetView* view) noexcept
{
    if (!IsQualifiedSceneTarget(view)) return {};
    D3D11_RENDER_TARGET_VIEW_DESC target{};
    view->GetDesc(&target);
    if (std::tuple{target.Format, target.ViewDimension, target.Texture2D.MipSlice} !=
        std::tuple{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_RTV_DIMENSION_TEXTURE2D, 0u}) return {};
    auto resource = ViewResource(view);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)))
        return {};
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.MipLevels != 1)
        return {};
    return resource;
}

bool ActualOriginalShader(ID3D11DeviceContext* context, const DrawObservation& observation) noexcept
{
    if (observation.currentReplacementActive || !observation.expectedBoundShader)
        return false;
    ComPtr<ID3D11PixelShader> actual;
    context->PSGetShader(&actual, nullptr, nullptr);
    return actual.Get() == observation.expectedBoundShader;
}

bool Unpredicated(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11Predicate> predicate;
    BOOL value{};
    context->GetPredication(&predicate, &value);
    return !predicate;
}

void ObserveOutputUavs(ID3D11DeviceContext* context) noexcept
{
    if (!context || context != g_rendererDevice.activeContext) return;
    if (!g_armed.load(std::memory_order_acquire) && !SharedUiStartupSdr()) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
        D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> uavs{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
        count, uavs.data());
    for (auto* uav : uavs)
    {
        if (uav)
        {
            (void)ObserveSharedUiCopy(ViewResource(uav).Get(), nullptr);
            uav->Release();
        }
    }
}

struct DrawTargets
{
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views;
    explicit DrawTargets(ID3D11DeviceContext* context) noexcept
    {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw{};
        context->OMGetRenderTargets(static_cast<UINT>(raw.size()), raw.data(), nullptr);
        for (std::size_t index = 0; index < raw.size(); ++index)
            views[index].Attach(raw[index]);
    }
    bool SinglePublic() const noexcept
    {
        return IsPresentationProxyTarget(views[0].Get()) &&
            std::ranges::all_of(views.begin() + 1, views.end(),
                [](const auto& view) { return !view; });
    }
    bool TemporalPublic() const noexcept
    {
        return views[0] &&
            !RenderTargetMatchesResource(views[0].Get(), g_presentation.presentationProxy.Get()) &&
            IsPresentationProxyTarget(views[1].Get()) &&
            std::ranges::all_of(views.begin() + 2, views.end(),
                [](const auto& view) { return !view; });
    }
    bool WritesPublic() const noexcept
    {
        return Writes(g_presentation.presentationProxy.Get());
    }
    bool Writes(ID3D11Resource* resource) const noexcept
    {
        if (!resource)
            return false;
        return std::ranges::any_of(views, [=](const auto& view) {
            return SameResourceObjects(ViewResource(view.Get()).Get(), resource);
        });
    }
};

bool FullResolutionUiSource(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(0, 1, &source);
    if (!source)
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    source->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    source->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture)))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return std::tuple{view.Format, view.Texture2D.MostDetailedMip, view.Texture2D.MipLevels,
               desc.Format, desc.Width, desc.Height, desc.ArraySize, desc.SampleDesc.Count} ==
        std::tuple{DXGI_FORMAT_R8G8B8A8_UNORM, 0u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM,
            g_presentation.width, g_presentation.height, 1u, 1u};
}

bool ConsumesOriginalLut(ID3D11DeviceContext* context, const DrawObservation& observation) noexcept
{
    const auto identity = g_originalLutIdentity.load(std::memory_order_acquire);
    if (!identity)
        return false;
    const auto* descriptor = FindShaderDescriptor(observation.currentPass);
    if (!descriptor)
        return false;
    ComPtr<ID3D11ShaderResourceView> input;
    context->PSGetShaderResources(descriptor->sceneInputSlot, 1, &input);
    if (!input) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    input->GetDesc(&view);
    if (std::tuple{view.Format, view.ViewDimension, view.Texture2D.MostDetailedMip, view.Texture2D.MipLevels} !=
        std::tuple{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0u, 1u}) return false;
    return CanonicalResourceIdentity(ViewResource(input.Get()).Get()) == identity;
}

bool OriginalProducerReady(ID3D11DeviceContext* context,
    const DrawObservation& observation) noexcept
{
    return ActualOriginalShader(context, observation) && Unpredicated(context);
}

bool TemporalProducerReady(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    return !g_content.complete && !g_content.videoProduced && targets.TemporalPublic() &&
        OriginalProducerReady(context, observation) && ConsumesOriginalLut(context, observation);
}

void ObserveTemporalProducer(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    if (!TemporalProducerReady(context, observation, targets))
    {
        Reject(SdrRejection::producer);
        return;
    }
    g_content.produced = true;
}

bool VideoProducerReady(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    return !g_content.publicDrawn && !g_content.produced && !g_content.complete &&
        targets.SinglePublic() && OriginalProducerReady(context, observation);
}

void ObserveVideoProducer(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    if (!VideoProducerReady(context, observation, targets))
    {
        Reject(SdrRejection::producer);
        return;
    }
    if (!ScaleSharedSdrVideo(context, g_content.videoWhite, g_content.white))
    {
        Reject(SdrRejection::videoScale);
        return;
    }
    g_content.videoProduced = g_content.produced = true;
}

void ObserveOriginalLut(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    if (targets.WritesPublic())
    {
        Reject(SdrRejection::producer);
        return;
    }
    if (OriginalProducerReady(context, observation))
        SetOriginalLut(OriginalLutResource(targets.views[0].Get()));
}

void ObserveKnownDraw(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    if (observation.currentPass == RenoDxShaderPass::lut)
    {
        ObserveOriginalLut(context, observation, targets);
        return;
    }
    if (!targets.WritesPublic())
        return;
    switch (observation.currentPass)
    {
    case RenoDxShaderPass::taa:
    case RenoDxShaderPass::taa2:
        ObserveTemporalProducer(context, observation, targets);
        break;
    case RenoDxShaderPass::video:
        ObserveVideoProducer(context, observation, targets);
        break;
    default:
        Reject(SdrRejection::producer);
        break;
    }
}

bool NativeUiReady(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    return targets.SinglePublic() && OriginalProducerReady(context, observation) &&
        FullResolutionUiSource(context);
}

void ObserveNativeUi(ID3D11DeviceContext* context, const DrawObservation& observation,
    const DrawTargets& targets) noexcept
{
    if (!targets.WritesPublic())
        return;
    if (!g_content.produced)
    {
        Reject(SdrRejection::producer);
        return;
    }
    if (!IsLateGameUiShader(observation.expectedBoundShader))
        return;
    if (!NativeUiReady(context, observation, targets))
    {
        Reject(SdrRejection::completion);
        return;
    }
    g_content.complete = true;
}

bool ArmReady(ID3D11DeviceContext* context) noexcept
{
    return !HasCurrentWriter(CurrentIdentity(context)) &&
        SdrPathRequested() && ContextReady(context) && PresentationProxyReady();
}

bool PositiveFinite(float value) noexcept
{
    return std::isfinite(value) && value > 0.0F;
}

bool CapturedFrameReady(const SdrFrameContent& content) noexcept
{
    return content.identity.generation && content.identity.contextGeneration &&
        PositiveFinite(content.white) && PositiveFinite(content.videoWhite);
}
} // namespace

void ClearWholePublicBuffer(ID3D11DeviceContext* context) noexcept
{
    // Predication affects ClearRenderTargetView; viewport and scissor do not.
    // Restore the game's predicate after establishing unconditional coverage.
    ComPtr<ID3D11Predicate> predicate;
    BOOL value{};
    context->GetPredication(&predicate, &value);
    context->SetPredication(nullptr, FALSE);
    constexpr float black[4]{0, 0, 0, 1};
    context->ClearRenderTargetView(g_presentation.presentationProxyRtv.Get(), black);
    context->SetPredication(predicate.Get(), value);
}

bool ArmSharedSdrFrame(ID3D11DeviceContext* context) noexcept
{
    if (CurrentEpoch(context))
        return !g_rejected.load(std::memory_order_acquire);
    if (!ArmReady(context))
        return false;
    SdrFrameContent captured{.identity = CurrentIdentity(context),
        .white = static_cast<float>(g_activeSettings.uiPaperWhiteNits),
        .videoWhite = static_cast<float>(g_activeSettings.videoWhiteNits)};
    if (!CapturedFrameReady(captured))
        return false;
    ScopedPresentationContext owned(context);
    if (!owned.Ready())
        return false;
    ResetSharedSdrFrame();
    const auto clearedRevision = InvalidateSharedUiPresentation();
    ClearWholePublicBuffer(context);
    captured.revision = g_arms.fetch_add(1, std::memory_order_relaxed) + 1;
    g_content = std::move(captured);
    g_ownerThread.store(g_content.identity.thread, std::memory_order_release);
    g_presentation.presentationProxyDomain = LinearColorDomain::vanillaSdrCodes;
    g_armed.store(true, std::memory_order_release);
    RefreshSharedUiStartupWhite(clearedRevision);
    return true;
}

bool SharedSdrFrameHasNativeWriter(ID3D11DeviceContext* context) noexcept
{
    return HasCurrentWriter(CurrentIdentity(context));
}

void ResetSharedSdrFrame() noexcept
{
    g_armed.store(false, std::memory_order_release);
    g_originalLutIdentity.store(0, std::memory_order_release);
    g_rejected.store(false, std::memory_order_release);
    g_frameRejection.store(SdrRejection::none, std::memory_order_release);
    g_ownerThread.store(0, std::memory_order_release);
    g_content = {};
}

void ObserveSharedSdrDrawEntry(ID3D11DeviceContext* context) noexcept
{
    if (context != g_rendererDevice.activeContext)
        return;
    (void)ArmSharedSdrFrame(context);
    MarkCurrentWriter(context);
    if (!g_armed.load(std::memory_order_acquire))
        return;
    if (!CurrentEpoch(context))
        Reject(SdrRejection::context);
}

void ObserveSharedSdrDraw(ID3D11DeviceContext* context, const DrawObservation& observation) noexcept
{
    // Readable SDR still needs mutation tracking after producer qualification fails.
    ObserveOutputUavs(context);
    if (!CurrentEpoch(context) || g_rejected.load(std::memory_order_acquire))
        return;
    const DrawTargets targets(context);
    if (targets.Writes(g_content.originalLut.Get()))
        SetOriginalLut({});
    if (observation.currentReplacementActive)
    {
        if (targets.WritesPublic())
            Reject(SdrRejection::replacement);
        return;
    }
    if (observation.currentPassKnown)
        ObserveKnownDraw(context, observation, targets);
    else
        ObserveNativeUi(context, observation, targets);
    g_content.publicDrawn |= targets.WritesPublic();
}

void ObserveSharedSdrCopy(ID3D11Resource* destination) noexcept
{
    if (SameResourceObjects(destination, g_presentation.presentationProxy.Get()))
    {
        MarkCurrentWriter(g_rendererDevice.activeContext);
        if (g_armed.load(std::memory_order_acquire))
            Reject(SdrRejection::mutation);
        return;
    }
    if (!g_armed.load(std::memory_order_acquire))
        return;
    auto identity = CanonicalResourceIdentity(destination);
    if (!identity)
    {
        Reject(SdrRejection::mutation);
        return;
    }
    g_originalLutIdentity.compare_exchange_strong(identity, 0, std::memory_order_acq_rel);
}

void ObserveSharedSdrClear(ID3D11RenderTargetView* target) noexcept
{
    ComPtr<ID3D11Resource> resource;
    if (target)
        target->GetResource(&resource);
    ObserveSharedSdrCopy(resource.Get());
}

bool SharedSdrFrameReady() noexcept
{
    return CurrentEpoch(g_rendererDevice.activeContext) && SdrPathRequested() &&
        !g_rejected.load(std::memory_order_acquire) && g_content.produced;
}

float SharedSdrPaperWhite() noexcept
{
    return CurrentEpoch(g_rendererDevice.activeContext) ? g_content.white : 0.0F;
}

bool TraceSharedSdrOutput(bool rendered, bool outputWritten, bool startupWritten) noexcept
{
    if (!g_armed.load(std::memory_order_acquire))
        return false;
    if (!FrameTraceRequested())
        return true;
    const auto decision = rendered && SharedSdrFrameReady() ?
        FrameTraceSharedUiDecision::hdrSdrContent : (startupWritten ?
            FrameTraceSharedUiDecision::startupSdr : FrameTraceSharedUiDecision::black);
    const auto white = decision == FrameTraceSharedUiDecision::startupSdr ?
        SharedUiStartupPaperWhite() : g_content.white;
    TraceSharedUiPresentation({.contentRevision = g_content.revision,
        .generation = g_content.identity.generation,
        .contextGeneration = g_content.identity.contextGeneration,
        .frame = g_content.identity.frame, .frozenWhite = white,
        .seeded = g_content.produced, .closed = g_content.complete,
        .nativeWritten = g_content.complete,
        .rejected = g_rejected.load(std::memory_order_acquire), .outputWritten = outputWritten,
        .assumed = startupWritten && SharedUiStartupAssumed(),
        .rejection = static_cast<std::uint32_t>(g_frameRejection.load(std::memory_order_acquire)),
        .decision = decision, .context = g_rendererDevice.activeContext,
        .selectedScene = g_presentation.presentationProxySrv.Get(),
        .publicBuffer = g_presentation.presentationProxy.Get(),
        .outputBuffer = g_presentation.presentationBackBuffer.Get()});
    return true;
}

void CompleteSharedSdrFrame(bool rendered) noexcept
{
    if (!g_armed.load(std::memory_order_acquire))
        return;
    if (rendered && SharedSdrFrameReady())
        g_presented.fetch_add(1, std::memory_order_relaxed);
    else
    {
        g_rejectedPresents.fetch_add(1, std::memory_order_relaxed);
        Reject(SdrRejection::incomplete);
    }
    ResetSharedSdrFrame();
}

void ReportSharedSdr(GraphicsDiagnosticSink sink)
{
    if (!g_configuration.developerMode) return;
    if (!g_configuration.productionRequested || !sink)
        return;
    constexpr std::array names{"none", "context", "replacement", "mutation", "producer",
        "completion", "incomplete", "video-scale"};
    sink(std::format("shared-sdr armed={} presented={} rejected={} last_rejection={}",
        g_arms.load(), g_presented.load(), g_rejectedPresents.load(),
        names[static_cast<std::size_t>(g_lastRejection.load())]));
}
} // namespace fo4hdr
