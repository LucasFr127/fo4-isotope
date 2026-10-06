// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "FrameTrace.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "RendererLifetime.hpp"
#include "GraphicsDiagnostics.hpp"
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {
std::vector<std::string> g_traceMessages;
void CaptureTraceMessage(std::string_view message) { g_traceMessages.emplace_back(message); }
struct TraceForwardProbe
{
    ID3D11DeviceContext* context{};
    ID3D11Resource* destination{};
    ID3D11Resource* source{};
    ID3D11RenderTargetView* target{};
    const FLOAT* color{};
    const D3D11_BOX* box{};
    std::array<UINT, 5> region{};
    UINT indices{}, startIndex{};
    INT baseVertex{};
    UINT copies{}, regions{}, clears{}, draws{};
    bool throwNext{};
};
TraceForwardProbe g_traceProbe;

void STDMETHODCALLTYPE TraceProbeCopy(ID3D11DeviceContext* context,
    ID3D11Resource* destination, ID3D11Resource* source)
{
    g_traceProbe.context = context;
    g_traceProbe.destination = destination;
    g_traceProbe.source = source;
    ++g_traceProbe.copies;
    if (g_traceProbe.throwNext) throw std::runtime_error("captured copy next threw");
}

void STDMETHODCALLTYPE TraceProbeRegion(ID3D11DeviceContext* context,
    ID3D11Resource* destination, UINT subresource, UINT x, UINT y, UINT z,
    ID3D11Resource* source, UINT sourceSubresource, const D3D11_BOX* box)
{
    g_traceProbe.context = context;
    g_traceProbe.destination = destination;
    g_traceProbe.source = source;
    g_traceProbe.region = {subresource, x, y, z, sourceSubresource};
    g_traceProbe.box = box;
    ++g_traceProbe.regions;
}

void STDMETHODCALLTYPE TraceProbeClear(ID3D11DeviceContext* context,
    ID3D11RenderTargetView* target, const FLOAT* color)
{
    g_traceProbe.context = context;
    g_traceProbe.target = target;
    g_traceProbe.color = color;
    ++g_traceProbe.clears;
}

void STDMETHODCALLTYPE TraceProbeDraw(ID3D11DeviceContext* context,
    UINT indices, UINT startIndex, INT baseVertex)
{
    g_traceProbe.context = context;
    g_traceProbe.indices = indices;
    g_traceProbe.startIndex = startIndex;
    g_traceProbe.baseVertex = baseVertex;
    ++g_traceProbe.draws;
}

void VerifyTraceForwarding(ID3D11DeviceContext* context, Texture& source, Texture& target)
{
    g_traceProbe = {};
    HookCopyResource(context, target.texture.Get(), source.texture.Get());
    FO4HDR_WARP_CHECK(g_traceProbe.copies == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.destination == target.texture.Get());
    FO4HDR_WARP_CHECK(g_traceProbe.source == source.texture.Get());
    const D3D11_BOX box{1, 0, 0, 3, 1, 1};
    HookCopySubresourceRegion(context, target.texture.Get(), 0, 2, 0, 0,
        source.texture.Get(), 0, &box);
    FO4HDR_WARP_CHECK(g_traceProbe.regions == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.destination == target.texture.Get());
    FO4HDR_WARP_CHECK(g_traceProbe.source == source.texture.Get());
    const std::array<UINT, 5> expectedRegion{0, 2, 0, 0, 0};
    FO4HDR_WARP_CHECK(g_traceProbe.region == expectedRegion && g_traceProbe.box == &box);
    const FLOAT color[]{0.25F, 0.5F, 0.75F, 1.0F};
    HookClearRenderTargetView(context, target.rtv.Get(), color);
    FO4HDR_WARP_CHECK(g_traceProbe.clears == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.target == target.rtv.Get() && g_traceProbe.color == color);
    HookDrawIndexed(context, 17, 3, -7);
    FO4HDR_WARP_CHECK(g_traceProbe.draws == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.indices == 17 && g_traceProbe.startIndex == 3);
    FO4HDR_WARP_CHECK(g_traceProbe.baseVertex == -7);
}

struct TraceContextScope
{
    ID3D11DeviceContext* context;
    ContextRecord& record{g_contexts.front()};
    DrawObservation observation{record};
    CopyResource copy{record.nextCopyResource};
    CopySubresourceRegion region{record.nextCopySubresourceRegion};
    ClearRenderTargetView clear{record.nextClearRenderTargetView};
    DrawIndexed draw{record.nextDrawIndexed};
    std::uint64_t exceptions{g_evidence.abiExceptionCount.load()};
    std::uint32_t failure{g_evidence.failure.load()};
    ID3D11DeviceContext* active{g_rendererDevice.activeContext};
    explicit TraceContextScope(ID3D11DeviceContext* value) : context(value)
    {
        if (!record.identity.Publish(context, 91)) throw std::runtime_error("Trace context already owned");
        static_cast<DrawObservation&>(record) = {};
        record.immediate = true;
        record.generation = 91;
        record.nextCopyResource = &TraceProbeCopy;
        record.nextCopySubresourceRegion = &TraceProbeRegion;
        record.nextClearRenderTargetView = &TraceProbeClear;
        record.nextDrawIndexed = &TraceProbeDraw;
        g_rendererDevice.activeContext = nullptr;
    }
    ~TraceContextScope()
    {
        (void)record.identity.Retire(context, 91);
        static_cast<DrawObservation&>(record) = observation;
        record.nextCopyResource = copy;
        record.nextCopySubresourceRegion = region;
        record.nextClearRenderTargetView = clear;
        record.nextDrawIndexed = draw;
        g_evidence.abiExceptionCount = exceptions;
        g_evidence.failure = failure;
        g_rendererDevice.activeContext = active;
    }
};

void VerifyTraceHookForwarding(Device& device)
{
    TraceContextScope scope(device.context.Get());
    auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE);
    auto target = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET);
    const bool requested = FrameTraceRequested();
    for (bool enabled : {false, true})
    {
        (void)InitializeFrameTrace(enabled);
        if (enabled) FO4HDR_WARP_CHECK(BeginFrameTrace());
        VerifyTraceForwarding(device.context.Get(), source, target);
        const auto exceptions = g_evidence.abiExceptionCount.load();
        g_traceProbe.throwNext = true;
        HookCopyResource(device.context.Get(), target.texture.Get(), source.texture.Get());
        FO4HDR_WARP_CHECK(g_traceProbe.copies == 2);
        FO4HDR_WARP_CHECK(g_evidence.abiExceptionCount.load() == exceptions + 1);
        RetireFrameTrace();
        ReportFrameTrace(CaptureTraceMessage);
    }
    (void)InitializeFrameTrace(requested);
    g_traceMessages.clear();
}

bool TraceContains(std::string_view value)
{
    return std::ranges::any_of(g_traceMessages, [=](const auto& line) {
        return line.find(value) != std::string::npos;
    });
}
}

void VerifyFrameTrace(Device& device)
{
    VerifyTraceHookForwarding(device);
    const bool requested = FrameTraceRequested();
    (void)InitializeFrameTrace(false);
    FO4HDR_WARP_CHECK(!BeginFrameTrace());
    g_traceMessages.clear();
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    ReportFrameTrace(CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    FO4HDR_WARP_CHECK(InitializeFrameTrace(true));
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    FO4HDR_WARP_CHECK(!BeginFrameTrace());
    DrawObservation observation{.immediate = true, .generation = 7};
    auto image = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    constexpr float clear[4]{0.25F, 0.5F, 1, 1};
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::drawIndexed, {6, 2, 0});
    TraceFrameCopy(device.context.Get(), observation,
        {FrameTraceKind::copy, image.texture.Get(), image.texture.Get()});
    TraceFrameClear(device.context.Get(), observation, image.rtv.Get(), clear);
    TraceFrameCommandList(device.context.Get(), observation, nullptr, FALSE);
    TraceNativeBoundary({}, true);
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, 17, 11);
    TraceFramePresent(chain.Get(), FrameTraceKind::nativePresent, {3, DXGI_PRESENT_DO_NOT_WAIT});
    for (unsigned frame = 0; frame < 8; ++frame)
        TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {3, DXGI_PRESENT_DO_NOT_WAIT});
    CheckHresult(chain->ResizeBuffers(0, 19, 13, DXGI_FORMAT_UNKNOWN, 0),
        "Resize with trace holding only numeric resource identities");
    ReportFrameTrace(CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=14 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("args=6/2/0"));
    FO4HDR_WARP_CHECK(TraceContains("mode=eight-frames"));
    const auto reported = g_traceMessages.size();
    ReportFrameTrace(CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.size() == reported);
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    RetireFrameTrace();
    ReportFrameTrace(CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("stop=4"));
    (void)InitializeFrameTrace(requested);
    CheckDebugMessages(device);
}
} // namespace fo4hdr::tests::gpu
