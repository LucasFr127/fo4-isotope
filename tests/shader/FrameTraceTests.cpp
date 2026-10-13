// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "TestHarness.hpp"
#include "FrameTrace.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "hooks/SwapChainHooks.hpp"
#include "RendererLifetime.hpp"
#include "DisplayDiscovery.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "LutProfileBindings.hpp"
#include "ShaderRegistry.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include "resources/DeveloperExposureResources.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "FO4Isotope/ui/HdrSettingsInput.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

std::vector<std::string> g_traceMessages;

void CaptureTraceMessage(std::string_view message)
{
    g_traceMessages.emplace_back(message);
}

struct TraceForwardProbe
{
    ID3D11DeviceContext* context{};
    ID3D11Resource* destination{};
    ID3D11Resource* source{};
    ID3D11RenderTargetView* target{};
    ID3D11UnorderedAccessView* uav{};
    ID3D11Buffer* arguments{};
    const FLOAT* color{};
    const UINT* integers{};
    const D3D11_BOX* box{};
    std::array<UINT, 5> region{};
    UINT indices{}, startIndex{};
    INT baseVertex{};
    UINT copies{}, regions{}, clears{}, draws{};
    std::array<UINT, 3> groups{};
    UINT offset{}, dispatches{}, indirectDispatches{}, uintClears{}, floatClears{};
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

void ThrowTraceProbe()
{
    if (g_traceProbe.throwNext) throw std::runtime_error("Captured native UAV next threw");
}

void STDMETHODCALLTYPE TraceProbeDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    g_traceProbe.context = context;
    g_traceProbe.groups = {x, y, z};
    ++g_traceProbe.dispatches;
    ThrowTraceProbe();
}

void STDMETHODCALLTYPE TraceProbeDispatchIndirect(ID3D11DeviceContext* context,
    ID3D11Buffer* arguments, UINT offset)
{
    g_traceProbe.context = context;
    g_traceProbe.arguments = arguments;
    g_traceProbe.offset = offset;
    ++g_traceProbe.indirectDispatches;
    ThrowTraceProbe();
}

void STDMETHODCALLTYPE TraceProbeClearUint(ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* target, const UINT* values)
{
    g_traceProbe.context = context;
    g_traceProbe.uav = target;
    g_traceProbe.integers = values;
    ++g_traceProbe.uintClears;
    ThrowTraceProbe();
}

void STDMETHODCALLTYPE TraceProbeClearFloat(ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* target, const FLOAT* values)
{
    g_traceProbe.context = context;
    g_traceProbe.uav = target;
    g_traceProbe.color = values;
    ++g_traceProbe.floatClears;
    ThrowTraceProbe();
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
    fo4hdr::Dispatch dispatch{record.nextDispatch};
    DispatchIndirect dispatchIndirect{record.nextDispatchIndirect};
    ClearUnorderedAccessViewUint clearUint{record.nextClearUnorderedAccessViewUint};
    ClearUnorderedAccessViewFloat clearFloat{record.nextClearUnorderedAccessViewFloat};
    std::uint64_t exceptions{g_evidence.abiExceptionCount.load()};
    std::uint32_t failure{g_evidence.failure.load()};
    ID3D11DeviceContext* activeContext{g_rendererDevice.activeContext};
    bool requested{FrameTraceRequested()};

    explicit TraceContextScope(ID3D11DeviceContext* value) : context(value)
    {
        FO4HDR_WARP_CHECK(record.identity.Publish(context, 91));
        record.immediate = true;
        record.generation = 91;
        record.nextCopyResource = &TraceProbeCopy;
        record.nextCopySubresourceRegion = &TraceProbeRegion;
        record.nextClearRenderTargetView = &TraceProbeClear;
        record.nextDrawIndexed = &TraceProbeDraw;
        record.nextDispatch = &TraceProbeDispatch;
        record.nextDispatchIndirect = &TraceProbeDispatchIndirect;
        record.nextClearUnorderedAccessViewUint = &TraceProbeClearUint;
        record.nextClearUnorderedAccessViewFloat = &TraceProbeClearFloat;
        g_rendererDevice.activeContext = nullptr;
    }

    ~TraceContextScope()
    {
        RetireFrameTrace();
        ReportFrameTrace(&CaptureTraceMessage);
        (void)InitializeFrameTrace(requested);
        FO4HDR_WARP_CHECK(record.identity.Retire(context, 91));
        static_cast<DrawObservation&>(record) = observation;
        record.nextCopyResource = copy;
        record.nextCopySubresourceRegion = region;
        record.nextClearRenderTargetView = clear;
        record.nextDrawIndexed = draw;
        record.nextDispatch = dispatch;
        record.nextDispatchIndirect = dispatchIndirect;
        record.nextClearUnorderedAccessViewUint = clearUint;
        record.nextClearUnorderedAccessViewFloat = clearFloat;
        g_evidence.abiExceptionCount = exceptions;
        g_evidence.failure = failure;
        g_rendererDevice.activeContext = activeContext;
        context->ClearState();
    }
};

void VerifyUavForwarding(ID3D11DeviceContext* context, Texture& target,
    ID3D11Buffer* arguments, bool throwNext)
{
    g_traceProbe = {};
    g_traceProbe.throwNext = throwNext;
    HookDispatch(context, 3, 5, 7);
    FO4HDR_WARP_CHECK(g_traceProbe.dispatches == 1 && g_traceProbe.context == context);
    const std::array<UINT, 3> groups{3, 5, 7};
    FO4HDR_WARP_CHECK(g_traceProbe.groups == groups);
    HookDispatchIndirect(context, arguments, 12);
    FO4HDR_WARP_CHECK(g_traceProbe.indirectDispatches == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.arguments == arguments && g_traceProbe.offset == 12);
    const UINT integers[]{1, 2, 3, 4};
    HookClearUnorderedAccessViewUint(context, target.uav.Get(), integers);
    FO4HDR_WARP_CHECK(g_traceProbe.uintClears == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.uav == target.uav.Get() && g_traceProbe.integers == integers);
    const FLOAT floats[]{0.25F, 0.5F, 0.75F, 1};
    HookClearUnorderedAccessViewFloat(context, target.uav.Get(), floats);
    FO4HDR_WARP_CHECK(g_traceProbe.floatClears == 1 && g_traceProbe.context == context);
    FO4HDR_WARP_CHECK(g_traceProbe.uav == target.uav.Get() && g_traceProbe.color == floats);
}

void VerifyNativeUavForwarding(Device& device, Texture& target)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = 24;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    ComPtr<ID3D11Buffer> arguments;
    CheckHresult(device.device->CreateBuffer(&description, nullptr, &arguments),
        "Create native UAV forwarding indirect arguments");
    const auto exceptions = g_evidence.abiExceptionCount.load();
    const auto failure = g_evidence.failure.load();
    VerifyUavForwarding(device.context.Get(), target, arguments.Get(), false);
    VerifyUavForwarding(device.context.Get(), target, arguments.Get(), true);
    FO4HDR_WARP_CHECK(g_evidence.abiExceptionCount.load() == exceptions + 4);
    g_rendererDevice.activeContext = device.context.Get();
    {
        ScopedPresentationContext owned(device.context.Get());
        FO4HDR_WARP_CHECK(owned.Ready());
        VerifyUavForwarding(device.context.Get(), target, arguments.Get(), false);
        VerifyUavForwarding(device.context.Get(), target, arguments.Get(), true);
    }
    g_rendererDevice.activeContext = nullptr;
    FO4HDR_WARP_CHECK(g_evidence.abiExceptionCount.load() == exceptions + 8);
    g_evidence.abiExceptionCount = exceptions;
    g_evidence.failure = failure;
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

std::string_view TraceField(std::string_view line, unsigned field)
{
    constexpr std::string_view prefix = "frame-trace event=";
    if (!line.starts_with(prefix)) return {};
    line.remove_prefix(prefix.size());
    for (unsigned index = 0; index < field; ++index)
    {
        const auto separator = line.find(',');
        if (separator == std::string_view::npos) return {};
        line.remove_prefix(separator + 1);
    }
    return line.substr(0, line.find_first_of(", "));
}

bool TraceContains(std::string_view text)
{
    return std::ranges::any_of(g_traceMessages, [&](const auto& line) {
        return line.find(text) != std::string::npos;
    });
}

void VerifyTraceBindings(Device& device, const Texture& source, const Texture& target,
    ID3D11PixelShader* shader)
{
    ComPtr<ID3D11PixelShader> actualShader;
    ComPtr<ID3D11RenderTargetView> actualTarget;
    ComPtr<ID3D11ShaderResourceView> actualInput;
    device.context->PSGetShader(&actualShader, nullptr, nullptr);
    device.context->OMGetRenderTargets(1, &actualTarget, nullptr);
    device.context->PSGetShaderResources(0, 1, &actualInput);
    FO4HDR_WARP_CHECK(actualShader.Get() == shader);
    FO4HDR_WARP_CHECK(actualTarget.Get() == target.rtv.Get());
    FO4HDR_WARP_CHECK(actualInput.Get() == source.srv.Get());
}

void VerifyTraceCallbacks(Device& device)
{
    TraceContextScope scope(device.context.Get());
    auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE);
    auto target = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
    const auto shaders = CreatePresentationShaders(*device.device.Get());
    auto* output = target.rtv.Get();
    auto* input = source.srv.Get();
    device.context->PSSetShader(shaders.pixel.Get(), nullptr, 0);
    device.context->OMSetRenderTargets(1, &output, nullptr);
    device.context->PSSetShaderResources(0, 1, &input);
    (void)InitializeFrameTrace(false);
    FO4HDR_WARP_CHECK(!BeginFrameTrace());
    VerifyNativeUavForwarding(device, target);
    VerifyTraceForwarding(device.context.Get(), source, target);
    FO4HDR_WARP_CHECK(InitializeFrameTrace(true));
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    FO4HDR_WARP_CHECK(scope.record.computeQueries.CanDispatch());
    scope.record.computeQueries.Begin(&scope);
    VerifyTraceForwarding(device.context.Get(), source, target);
    scope.record.computeQueries.End(&scope);
    {
        ScopedFrameTraceOwnedCall owned;
        constexpr FLOAT color[]{0.0F, 0.0F, 0.0F, 1.0F};
        HookClearRenderTargetView(device.context.Get(), target.rtv.Get(), color);
    }
    ArmorUiScopeToken token{};
    token.owner = ArmorUiOwner::other;
    token.threadId = GetCurrentThreadId();
    token.serial = 73;
    token.targetArgument = 5;
    token.depth = 2;
    constexpr std::string_view name = "NativeHUD";
    std::ranges::copy(name, token.rendererName.begin());
    TraceNativeBoundary(token, true);
    TraceNativeBoundary(token, false);
    VerifyTraceBindings(device, source, target, shaders.pixel.Get());
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    RetireFrameTrace();
    VerifyTraceForwarding(device.context.Get(), source, target);
    VerifyTraceBindings(device, source, target, shaders.pixel.Get());
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.size() == 9);
    FO4HDR_WARP_CHECK(TraceContains("events=7 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("version=2"));
    FO4HDR_WARP_CHECK((std::stoul(std::string(TraceField(g_traceMessages[4], 13))) & 128U) != 0);
    FO4HDR_WARP_CHECK((std::stoul(std::string(TraceField(g_traceMessages[5], 13))) & 128U) == 0);
    FO4HDR_WARP_CHECK((std::stoul(std::string(TraceField(g_traceMessages[5], 13))) & 32U) != 0);
    FO4HDR_WARP_CHECK((std::stoul(std::string(TraceField(g_traceMessages[3], 13))) & 32U) == 0);
    FO4HDR_WARP_CHECK(TraceContains("coverage=submission-only"));
    FO4HDR_WARP_CHECK(TraceContains("resource-addresses=non-owning-reusable"));
    FO4HDR_WARP_CHECK(TraceContains("args=0/2/0/0/0/1/1/0/0/3/1/1"));
    FO4HDR_WARP_CHECK(TraceContains("args=17/3/4294967289/0"));
    FO4HDR_WARP_CHECK(TraceContains("name=NativeHUD"));
    const auto resource = std::format("r1={:x}/{}/{}/{}/1/0/0/0/{}/1/1 ",
        reinterpret_cast<std::uintptr_t>(source.texture.Get()), kWidth, kHeight,
        static_cast<unsigned>(DXGI_FORMAT_R16G16B16A16_FLOAT), static_cast<unsigned>(D3D11_BIND_SHADER_RESOURCE));
    FO4HDR_WARP_CHECK(TraceContains(resource));
}

void CheckSharedUiTraceReport(const FrameTraceSharedUiSnapshot& snapshot,
    std::uintptr_t outputIdentity)
{
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.size() == 8);
    FO4HDR_WARP_CHECK(TraceContains("events=6 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("shared-ui-kind=13 shared-ui-context-generation=content"));
    for (std::size_t index = 0; index < 3; ++index)
    {
        const auto& line = g_traceMessages[index * 2 + 1];
        FO4HDR_WARP_CHECK(TraceField(line, 2) == std::to_string(index + 1));
        FO4HDR_WARP_CHECK(TraceField(line, 3) ==
            std::to_string(static_cast<unsigned>(FrameTraceKind::sharedUiPresentation)));
        FO4HDR_WARP_CHECK(TraceField(line, 5) == std::to_string(g_evidence.resourceGeneration.load()));
        FO4HDR_WARP_CHECK(TraceField(line, 7) == std::to_string(snapshot.contextGeneration));
    }
    const auto white = std::bit_cast<std::uint32_t>(snapshot.frozenWhite);
    const auto menuGeneration = HdrSettingsNativeMenuGeneration();
    const auto menu = static_cast<std::uint32_t>(ReadHdrSettingsNativeMenuState());
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=0/0/23/{}/2/1/4/3/8/7/{}/{}",
        white, menuGeneration, menu)));
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=1/0/48/{}/2/1/4/3/8/7", white)));
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=2/8/11/{}/2/1/4/3/8/7", white)));
    ComPtr<ID3D11Resource> selected;
    snapshot.selectedScene->GetResource(&selected);
    FO4HDR_WARP_CHECK(TraceContains(std::format("r0={:x}/{}/{}/{}/1/{}",
        reinterpret_cast<std::uintptr_t>(selected.Get()), kWidth, kHeight,
        static_cast<unsigned>(DXGI_FORMAT_R16G16B16A16_FLOAT),
        static_cast<unsigned>(DXGI_FORMAT_R16G16B16A16_FLOAT))));
    FO4HDR_WARP_CHECK(TraceContains(std::format("r1={:x}/{}/{}/{}/1/0",
        reinterpret_cast<std::uintptr_t>(snapshot.publicBuffer), kWidth, kHeight,
        static_cast<unsigned>(DXGI_FORMAT_R16G16B16A16_FLOAT))));
    FO4HDR_WARP_CHECK(TraceContains(std::format("r2={:x}/37/19/{}/1/0",
        outputIdentity, static_cast<unsigned>(DXGI_FORMAT_R10G10B10A2_UNORM))));
}

void VerifySharedUiTrace(Device& device)
{
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, 37, 19);
    auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE);
    auto target = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11Texture2D> output;
    CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&output)), "Get shared UI trace output");
    const auto outputIdentity = reinterpret_cast<std::uintptr_t>(output.Get());
    FrameTraceSharedUiSnapshot snapshot{
        .contentRevision = 0x100000002ULL, .generation = 0x300000004ULL,
        .contextGeneration = 0x500000006ULL, .frame = 0x700000008ULL, .frozenWhite = 203,
        .seeded = true, .closed = true, .nativeWritten = true, .outputWritten = true,
        .decision = FrameTraceSharedUiDecision::hdr, .context = device.context.Get(),
        .selectedScene = source.srv.Get(), .publicBuffer = target.texture.Get(), .outputBuffer = output.Get()};
    (void)InitializeFrameTrace(false);
    TraceSharedUiPresentation(snapshot);
    FO4HDR_WARP_CHECK(InitializeFrameTrace(true));
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    TraceSharedUiPresentation(snapshot);
    TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {});
    snapshot.decision = FrameTraceSharedUiDecision::startupSdr;
    snapshot.seeded = snapshot.closed = snapshot.nativeWritten = false;
    snapshot.assumed = true;
    TraceSharedUiPresentation(snapshot);
    TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {});
    snapshot.decision = FrameTraceSharedUiDecision::black;
    snapshot.seeded = snapshot.closed = snapshot.rejected = true;
    snapshot.outputWritten = snapshot.assumed = false;
    snapshot.rejection = 8;
    TraceSharedUiPresentation(snapshot);
    TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {});
    RetireFrameTrace();
    output.Reset();
    CheckHresult(chain->ResizeBuffers(2, 41, 23, DXGI_FORMAT_UNKNOWN, 0),
        "Resize with shared UI trace retaining only numeric resource identities");
    CheckSharedUiTraceReport(snapshot, outputIdentity);
}

struct ArmedTraceScope
{
    bool productionRequested{g_configuration.productionRequested};

    ArmedTraceScope() { g_configuration.productionRequested = true; }
    ~ArmedTraceScope()
    {
        CancelFrameTrace();
        ReportFrameTrace(&CaptureTraceMessage);
        g_configuration.productionRequested = productionRequested;
    }
};

struct AutomaticTraceProbe
{
    std::mutex mutex;
    std::vector<std::string> messages;
    std::atomic<DWORD> thread{};
    std::atomic<unsigned> failedCalls{};
    std::atomic<bool> block{}, entered{}, release{}, completed{}, timedOut{};
};
AutomaticTraceProbe g_automaticTrace;

void CaptureAutomaticTraceMessage(std::string_view message)
{
    g_automaticTrace.thread.store(GetCurrentThreadId());
    if (g_automaticTrace.block.load() && message.starts_with("frame-trace begin"))
    {
        g_automaticTrace.entered.store(true);
        g_automaticTrace.timedOut.store(!test::WaitUntil([] { return g_automaticTrace.release.load(); }));
    }
    {
        const std::lock_guard lock(g_automaticTrace.mutex);
        g_automaticTrace.messages.emplace_back(message);
    }
    if (message.starts_with("frame-trace end")) g_automaticTrace.completed.store(true);
}

void ThrowAutomaticTraceMessage(std::string_view)
{
    g_automaticTrace.failedCalls.fetch_add(1);
    throw std::runtime_error("automatic trace report failure");
}

struct AutomaticTraceScope
{
    AutomaticTraceScope()
    {
        Reset();
        SetFrameTraceAutoReportSink(&CaptureAutomaticTraceMessage);
    }
    ~AutomaticTraceScope()
    {
        SetFrameTraceAutoReportSink(nullptr);
        g_automaticTrace.release.store(true);
        FO4HDR_WARP_CHECK(test::WaitUntil([] { return !FrameTraceAutoReportPending(); }));
    }
    void Reset()
    {
        if (!test::WaitUntil([] { return !FrameTraceAutoReportPending(); }))
            throw std::runtime_error("Automatic trace reporter did not finish before reset");
        g_traceMessages.clear();
        const std::lock_guard lock(g_automaticTrace.mutex);
        g_automaticTrace.messages.clear();
        g_automaticTrace.thread.store(0);
        g_automaticTrace.failedCalls.store(0);
        g_automaticTrace.block.store(false);
        g_automaticTrace.entered.store(false);
        g_automaticTrace.release.store(false);
        g_automaticTrace.completed.store(false);
        g_automaticTrace.timedOut.store(false);
    }
    bool Wait()
    {
        const bool completed = test::WaitUntil([] { return g_automaticTrace.completed.load(); });
        const bool idle = test::WaitUntil([] { return !FrameTraceAutoReportPending(); });
        FO4HDR_WARP_CHECK(completed);
        FO4HDR_WARP_CHECK(idle);
        FO4HDR_WARP_CHECK(!g_automaticTrace.timedOut.load());
        FO4HDR_WARP_CHECK(g_automaticTrace.thread.load() != GetCurrentThreadId());
        if (!completed || !idle) return false;
        const std::lock_guard lock(g_automaticTrace.mutex);
        g_traceMessages = g_automaticTrace.messages;
        return true;
    }
};

void VerifyAutomaticTraceCompletion()
{
    ArmedTraceScope armed;
    (void)InitializeFrameTrace(false);
    AutomaticTraceScope report;
    FO4HDR_WARP_CHECK(!BeginFrameTrace() && !ArmFrameTrace());
    FO4HDR_WARP_CHECK(!FrameTraceAutoReportPending());
    FO4HDR_WARP_CHECK(InitializeFrameTrace(true));
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    for (unsigned frame = 0; frame < 8; ++frame)
        TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    if (!report.Wait()) return;
    FO4HDR_WARP_CHECK(g_traceMessages.size() == 10);
    FO4HDR_WARP_CHECK(TraceContains("events=8 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("mode=eight-frames trigger=none incomplete=false cancelled=false"));
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
}

void VerifyAutomaticTraceCancelAndRestart()
{
    ArmedTraceScope armed;
    AutomaticTraceScope report;
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceNativeBoundary({}, true);
    CancelFrameTrace();
    if (!report.Wait()) return;
    FO4HDR_WARP_CHECK(TraceContains("events=1 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=none incomplete=false cancelled=true"));
    report.Reset();
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceSharedUiPresentation({});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {3, DXGI_PRESENT_DO_NOT_WAIT});
    if (!report.Wait()) return;
    FO4HDR_WARP_CHECK(TraceContains("events=2 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=black incomplete=false cancelled=false"));
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=3/{}/0/0", DXGI_PRESENT_DO_NOT_WAIT)));
}

void VerifyAutomaticTraceUnregister()
{
    ArmedTraceScope armed;
    AutomaticTraceScope report;
    g_automaticTrace.block.store(true);
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    TraceNativeBoundary({}, true);
    RetireFrameTrace();
    FO4HDR_WARP_CHECK(test::WaitUntil([] { return g_automaticTrace.entered.load(); }));
    SetFrameTraceAutoReportSink(nullptr);
    FO4HDR_WARP_CHECK(FrameTraceAutoReportPending());
    FO4HDR_WARP_CHECK(!BeginFrameTrace() && !ArmFrameTrace());
    RetireFrameTrace();
    g_automaticTrace.release.store(true);
    if (!report.Wait()) return;
    FO4HDR_WARP_CHECK(g_traceMessages.size() == 3);
    FO4HDR_WARP_CHECK(TraceContains("events=1 stop=4 dropped=0"));
    report.Reset();
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    TraceNativeBoundary({}, false);
    RetireFrameTrace();
    FO4HDR_WARP_CHECK(!FrameTraceAutoReportPending());
    FO4HDR_WARP_CHECK(!g_automaticTrace.completed.load());
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=1 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(g_automaticTrace.thread.load() == 0);
}

void VerifyAutomaticTraceThrowingSink()
{
    ArmedTraceScope armed;
    AutomaticTraceScope report;
    SetFrameTraceAutoReportSink(&ThrowAutomaticTraceMessage);
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    TraceNativeBoundary({}, true);
    RetireFrameTrace();
    const bool drained = test::WaitUntil([] {
        return g_automaticTrace.failedCalls.load() != 0 && !FrameTraceAutoReportPending();
    });
    FO4HDR_WARP_CHECK(drained);
    if (!drained) return;
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    FO4HDR_WARP_CHECK(test::WaitUntil([] { return !FrameTraceAutoReportPending(); }));
    FO4HDR_WARP_CHECK(g_automaticTrace.failedCalls.load() == 1);
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=1 stop=4 dropped=0"));
}

void VerifyNonProductionMutationHooks(Device& device)
{
    const bool requested = FrameTraceRequested();
    (void)InitializeFrameTrace(false);
    auto native = CreateNativeContextDevice(device);
    for (const bool reference : {false, true})
    {
        PresentationContextFixture fixture(native);
        g_configuration.productionRequested = false;
        g_configuration.enabled = reference;
        g_configuration.developerMode = reference;
        g_configuration.developerHdr10Reference = reference;
        fixture.InstallHooks();
    }
    (void)InitializeFrameTrace(requested);
}

void VerifyArmedTraceRollingAndTrigger()
{
    ArmedTraceScope scope;
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    FO4HDR_WARP_CHECK(!BeginFrameTrace() && !ArmFrameTrace());
    FrameTraceSharedUiSnapshot snapshot{};
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        snapshot.decision = static_cast<FrameTraceSharedUiDecision>(frame % 2);
        TraceSharedUiPresentation(snapshot);
        TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    }
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    snapshot.decision = FrameTraceSharedUiDecision::hdrSdrContent;
    TraceSharedUiPresentation(snapshot);
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    snapshot.decision = FrameTraceSharedUiDecision::black;
    TraceSharedUiPresentation(snapshot);
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {1, DXGI_PRESENT_TEST});
    CancelFrameTrace();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    TraceFramePresent(nullptr, FrameTraceKind::nextPresent, {3, DXGI_PRESENT_DO_NOT_WAIT});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {3, DXGI_PRESENT_DO_NOT_WAIT});
    FO4HDR_WARP_CHECK(!BeginFrameTrace() && !ArmFrameTrace());
    TraceSharedUiPresentation(snapshot);
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.size() == 32);
    FO4HDR_WARP_CHECK(TraceContains("events=30 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("mode=first-black trigger=black incomplete=false cancelled=false"));
    for (std::size_t index = 27; index <= 30; ++index)
        FO4HDR_WARP_CHECK(TraceField(g_traceMessages[index], 2) == "14");
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[30], 3) ==
        std::to_string(static_cast<unsigned>(FrameTraceKind::presentResult)));
}

void ThrowTraceReport(std::string_view)
{
    throw std::runtime_error("trace report retry");
}

void VerifyArmedTraceCancelAndReportRetry()
{
    ArmedTraceScope scope;
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceNativeBoundary({}, true);
    CancelFrameTrace();
    bool threw{};
    try { ReportFrameTrace(&ThrowTraceReport); }
    catch (const std::runtime_error&) { threw = true; }
    FO4HDR_WARP_CHECK(threw);
    FO4HDR_WARP_CHECK(!BeginFrameTrace() && !ArmFrameTrace());
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=1 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("mode=first-black trigger=none incomplete=false cancelled=true"));
    g_configuration.productionRequested = false;
    FO4HDR_WARP_CHECK(!ArmFrameTrace());
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    CancelFrameTrace();
    TraceNativeBoundary({}, true);
    RetireFrameTrace();
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("mode=eight-frames trigger=none incomplete=false cancelled=false"));
}

void VerifyTraceHotkeyRearmsAfterCancellation()
{
    ArmedTraceScope scope;
    ProcessFrameTraceHotkey(false, true);
    ProcessFrameTraceHotkey(true, true);
    TraceNativeBoundary({}, true);
    ProcessFrameTraceHotkey(false, true);
    ProcessFrameTraceHotkey(true, true);
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=1 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=none incomplete=false cancelled=true"));
    ProcessFrameTraceHotkey(false, true);
    ProcessFrameTraceHotkey(true, true);
    TraceSharedUiPresentation({});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    ProcessFrameTraceHotkey(false, true);
    ProcessFrameTraceHotkey(true, true);
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=2 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=black incomplete=false cancelled=false"));
    ProcessFrameTraceHotkey(false, true);
}

void VerifyTraceHotkeyModeAndThreadMigration()
{
    ArmedTraceScope scope;
    g_configuration.productionRequested = false;
    ProcessFrameTraceHotkey(false, true);
    ProcessFrameTraceHotkey(true, true);
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    RetireFrameTrace();
    ReportFrameTrace(&CaptureTraceMessage);
    ProcessFrameTraceHotkey(true, false);
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    RetireFrameTrace();
    ReportFrameTrace(&CaptureTraceMessage);
    g_configuration.productionRequested = true;
    ProcessFrameTraceHotkey(false, false);
    ProcessFrameTraceHotkey(true, false);
    ProcessFrameTraceHotkey(true, true);
    TraceNativeBoundary({}, true);
    RetireFrameTrace();
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("mode=eight-frames trigger=none"));
    ProcessFrameTraceHotkey(false, true);
    ProcessFrameTraceHotkey(true, true);
    std::jthread([] { ProcessFrameTraceHotkey(true, true); }).join();
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    TraceSharedUiPresentation({});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("mode=first-black trigger=black"));
    ProcessFrameTraceHotkey(false, true);
}

void VerifyArmedTraceRetirement(Device& device)
{
    ArmedTraceScope scope;
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, 37, 19);
    ComPtr<ID3D11Texture2D> output;
    CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&output)), "Get armed trace output");
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceNativeBoundary({}, true);
    RetireFrameTrace();
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    FrameTraceSharedUiSnapshot snapshot{.outputBuffer = output.Get()};
    TraceSharedUiPresentation(snapshot);
    RetireFrameTrace();
    TraceFramePresent(chain.Get(), FrameTraceKind::nextPresent, {1, 0});
    TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {1, 0, DXGI_ERROR_DEVICE_REMOVED});
    output.Reset();
    CheckHresult(chain->ResizeBuffers(2, 41, 23, DXGI_FORMAT_UNKNOWN, 0),
        "Resize with armed trace retaining only numeric resource identities");
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=4 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=black incomplete=true cancelled=false"));
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=1/0/{}/0",
        static_cast<unsigned>(DXGI_ERROR_DEVICE_REMOVED))));
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[2], 3) ==
        std::to_string(static_cast<unsigned>(FrameTraceKind::sharedUiPresentation)));
}

void VerifyArmedTraceBoundedHistory()
{
    ArmedTraceScope scope;
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    for (unsigned event = 0; event < 65536; ++event) TraceNativeBoundary({}, true);
    FO4HDR_WARP_CHECK(!BeginFrameTrace() && !ArmFrameTrace());
    TraceSharedUiPresentation({});
    CancelFrameTrace();
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=65536 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("mode=first-black trigger=black incomplete=true cancelled=false"));
    FO4HDR_WARP_CHECK(TraceContains("overwritten=2 first_sequence=3"));
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[65536], 0) == "65538");
}

void VerifyArmedTraceCancelRace()
{
    ArmedTraceScope scope;
    for (unsigned attempt = 0; attempt < 32; ++attempt)
    {
        FO4HDR_WARP_CHECK(ArmFrameTrace());
        std::atomic<bool> start{};
        std::atomic<bool> started{};
        std::jthread black([&] {
            started.store(test::WaitUntil([&] { return start.load(); }));
            if (started.load()) TraceSharedUiPresentation({});
        });
        std::jthread cancel([&] {
            start.store(true);
            CancelFrameTrace();
        });
        black.join();
        cancel.join();
        FO4HDR_WARP_CHECK(started.load());
        TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
        g_traceMessages.clear();
        ReportFrameTrace(&CaptureTraceMessage);
        const bool cancelled = TraceContains("trigger=none incomplete=false cancelled=true");
        // Cancellation can retire between the event writer's two recording-state checks.
        const bool cancellationDropped = TraceContains("trigger=none incomplete=true cancelled=true") &&
            TraceContains("events=0 stop=4 dropped=1");
        const bool triggered = TraceContains("trigger=black incomplete=false cancelled=false");
        FO4HDR_WARP_CHECK(cancelled || cancellationDropped || triggered);
        if (triggered) FO4HDR_WARP_CHECK(TraceContains("events=2 stop=2 dropped=0"));
    }
}

void VerifyArmedTraceLifecycleReset()
{
    ArmedTraceScope scope;
    const auto saved = ReadGraphicsLifecycle();
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceNativeBoundary({}, true);
    PublishGraphicsLifecycle(saved.gameplayReady, true);
    const auto reset = ReadGraphicsLifecycle().resetEpoch;
    TraceSharedUiPresentation({});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=3 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=black incomplete=true cancelled=false"));
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[2], 6) == std::to_string(reset));
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    RetireFrameTrace();
    TraceNativeBoundary({}, true);
    TraceSharedUiPresentation({.decision = FrameTraceSharedUiDecision::hdr});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    TraceSharedUiPresentation({});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {});
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=5 stop=2 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=black incomplete=false cancelled=false"));
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[4], 2) == "2");
    PublishGraphicsLifecycle(saved.gameplayReady, false);
    PublishGraphicsPresentationBlocked(saved.presentationBlocked);
}

void VerifyArmedTraceFailedPresent()
{
    ArmedTraceScope scope;
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceNativeBoundary({}, true);
    TraceFramePresent(nullptr, FrameTraceKind::presentResult, {1, 0, E_FAIL});
    FO4HDR_WARP_CHECK(!ArmFrameTrace());
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=2 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=none incomplete=true cancelled=false"));
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=1/0/{}/0", static_cast<unsigned>(E_FAIL))));
}

class ReentrantTraceResource final : public ID3D11Resource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** result) override
    {
        *result = nullptr;
        TraceSharedUiPresentation({});
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override
    {
        *dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
};

void VerifyContendedBlackTraceRetainsPresentResult()
{
    ArmedTraceScope scope;
    ReentrantTraceResource resource;
    FO4HDR_WARP_CHECK(ArmFrameTrace());
    TraceFrameCopy(nullptr, {}, {.destination = &resource});
    CancelFrameTrace();
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(g_traceMessages.empty());
    TraceFramePresent(nullptr, FrameTraceKind::nextPresent, {2, DXGI_PRESENT_DO_NOT_WAIT});
    TraceFramePresent(nullptr, FrameTraceKind::presentResult,
        {2, DXGI_PRESENT_DO_NOT_WAIT, DXGI_STATUS_OCCLUDED});
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=3 stop=2 dropped=1"));
    FO4HDR_WARP_CHECK(TraceContains("trigger=black incomplete=true cancelled=false"));
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[3], 3) ==
        std::to_string(static_cast<unsigned>(FrameTraceKind::presentResult)));
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=2/{}/{}/0",
        DXGI_PRESENT_DO_NOT_WAIT, static_cast<unsigned>(DXGI_STATUS_OCCLUDED))));
}

void CheckTraceShaderEvent(std::size_t index, const RenoDxShaderDescriptor* descriptor,
    ID3D11PixelShader* shader, const Texture& input)
{
    const auto& line = g_traceMessages[index + 1];
    FO4HDR_WARP_CHECK(TraceField(line, 9) == std::format("{:x}", reinterpret_cast<std::uintptr_t>(shader)));
    FO4HDR_WARP_CHECK(TraceField(line, 10) == "0000abcd");
    const auto pass = descriptor ? static_cast<unsigned>(descriptor->pass) + 1 : 0;
    const auto slot = descriptor ? descriptor->sceneInputSlot : 0;
    FO4HDR_WARP_CHECK(TraceField(line, 11) == std::to_string(pass));
    FO4HDR_WARP_CHECK(TraceField(line, 12) == std::to_string(slot));
    FO4HDR_WARP_CHECK(line.find(std::format("r2={:x}/", reinterpret_cast<std::uintptr_t>(input.texture.Get()))) !=
        std::string::npos);
}

void VerifyTraceReplacementAttribution(Device& device)
{
    auto zero = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE);
    auto one = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE);
    const auto actual = CreatePresentationShaders(*device.device.Get());
    const auto other = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiSyntheticPs);
    constexpr std::uint32_t crc = 0xabcd;
    CheckHresult(actual.pixel->SetPrivateData(kFrameTraceShaderCrc, sizeof(crc), &crc), "Set trace CRC");
    device.context->PSSetShader(actual.pixel.Get(), nullptr, 0);
    const std::array inputs{zero.srv.Get(), one.srv.Get()};
    device.context->PSSetShaderResources(0, 2, inputs.data());
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    DrawObservation observation{.immediate = true, .currentPass = RenoDxShaderPass::toneMapA,
        .currentPassKnown = true, .currentReplacementActive = true, .expectedBoundShader = actual.pixel.Get()};
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    observation.expectedBoundShader = other.Get();
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    observation.expectedBoundShader = nullptr;
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    observation.expectedBoundShader = actual.pixel.Get();
    observation.currentPassKnown = false;
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    observation.currentPassKnown = true;
    observation.currentReplacementActive = false;
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    observation.currentReplacementActive = true;
    observation.currentPass = static_cast<RenoDxShaderPass>(255);
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    auto* shader = actual.pixel.Get();
    const auto* original = FindShaderDescriptor(RenoDxShaderPass::lut);
    CreatedShaderIdentity{.descriptor = original, .diagnosticCrc = crc}.Register(S_OK, &shader);
    observation.currentPass = RenoDxShaderPass::toneMapA;
    TraceFrameDraw(device.context.Get(), observation, FrameTraceKind::draw, {});
    RetireFrameTrace();
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=7 stop=4 dropped=0"));
    CheckTraceShaderEvent(0, FindShaderDescriptor(RenoDxShaderPass::toneMapA), shader, one);
    for (std::size_t index = 1; index <= 5; ++index) CheckTraceShaderEvent(index, nullptr, shader, zero);
    CheckTraceShaderEvent(6, original, shader, zero);
    ResetShaderRegistry();
    device.context->ClearState();
}

struct TracePresentProbe
{
    IDXGISwapChain* chain{};
    UINT interval{}, flags{}, calls{};
};
TracePresentProbe g_tracePresent;

HRESULT STDMETHODCALLTYPE TraceProbePresent(IDXGISwapChain* chain, UINT interval, UINT flags)
{
    g_tracePresent = {chain, interval, flags, g_tracePresent.calls + 1};
    return DXGI_STATUS_OCCLUDED;
}

void VerifyTracePresentForwarding(IDXGISwapChain* chain, UINT flags)
{
    g_tracePresent = {};
    FO4HDR_WARP_CHECK(HookPresent(chain, 3, flags) == DXGI_STATUS_OCCLUDED);
    FO4HDR_WARP_CHECK(g_tracePresent.chain == chain && g_tracePresent.calls == 1);
    FO4HDR_WARP_CHECK(g_tracePresent.interval == 3 && g_tracePresent.flags == flags);
}

template <class Value, std::size_t Count>
struct TraceAtomicRestore
{
    std::array<std::atomic<Value>*, Count> fields;
    std::array<Value, Count> saved{};

    explicit TraceAtomicRestore(std::array<std::atomic<Value>*, Count> values) : fields(values)
    {
        for (std::size_t index = 0; index < Count; ++index) saved[index] = fields[index]->load();
    }
    ~TraceAtomicRestore()
    {
        for (std::size_t index = 0; index < Count; ++index) fields[index]->store(saved[index]);
    }
};

struct TraceOwnedPresentScope
{
    RendererDevice renderer{g_rendererDevice};
    PluginConfiguration configuration{g_configuration};
    HdrRendererLifecycle lifecycle{g_lifecycle};
    PipelineHeartbeat heartbeat{g_heartbeat};
    PresentationResources presentation{g_presentation};
    SceneResources scene{g_scene};
    UiTransportResources ui{g_uiTransport};
    ArmorCompositionResources armor{g_armor};
    DeveloperExposureResources exposure{g_developerExposure};
    HMONITOR monitor{ActiveDisplayMonitor()};
    int hdrState{ActiveDisplayHdrState()};
    std::uint64_t frame{g_frame}, inspectorFrame{g_lutInspectorFrame};
    std::uint32_t inspectorSlots{g_lutInspectorValidatedSlots};
    TraceAtomicRestore<std::uint64_t, 7> counts{std::array{&g_evidence.presentCount, &g_evidence.sdrFallbackFrameCount,
        &g_evidence.heartbeatIncompleteFrameCount, &g_evidence.heartbeatInvalidFrameCount,
        &g_evidence.resourceGeneration, &g_evidence.gameResetEpoch, &g_evidence.colorDomainGeneration}};
    TraceAtomicRestore<bool, 9> states{std::array{&g_referenceHealthy, &g_gameplayReadyApplied,
        &g_currentFrameQualifiedDrawThreadConflict, &g_liveSettingsThreadCompatible,
        &g_evidence.heartbeatReady, &g_evidence.gameplayReady, &g_evidence.hdrChainActive,
        &g_evidence.productionActive, &g_evidence.presentationPaused}};
    TraceAtomicRestore<DWORD, 2> threads{std::array{&g_currentFrameQualifiedDrawThread, &g_observedPresentThread}};
    TraceAtomicRestore<std::uint32_t, 1> domains{std::array{&g_evidence.colorDomain}};

    explicit TraceOwnedPresentScope(Device& device)
    {
        g_rendererDevice.activeDevice = device.device.Get();
        g_rendererDevice.activeContext = device.context.Get();
        g_rendererDevice.activeContext1.Reset();
        g_configuration.enabled = false;
        g_configuration.productionRequested = false;
        g_configuration.developerHdr10Reference = false;
        g_configuration.measurePerformance = false;
        g_lifecycle = {};
        FO4HDR_WARP_CHECK(g_lifecycle.BindDeviceAndSwapChain());
        const auto game = ReadGraphicsLifecycle();
        (void)g_lifecycle.OnGameReset(game.resetEpoch);
        g_gameplayReadyApplied = game.gameplayReady && !game.presentationBlocked;
        g_presentation = {};
        g_scene = {};
        g_uiTransport = {};
        g_armor = {};
        g_developerExposure = {};
        PublishActiveDisplay(nullptr, -1);
    }

    ~TraceOwnedPresentScope()
    {
        g_rendererDevice = renderer;
        g_configuration = configuration;
        g_lifecycle = lifecycle;
        g_heartbeat = heartbeat;
        g_presentation = presentation;
        g_scene = scene;
        g_uiTransport = ui;
        g_armor = armor;
        g_developerExposure = exposure;
        g_frame = frame;
        g_lutInspectorFrame = inspectorFrame;
        g_lutInspectorValidatedSlots = inspectorSlots;
        PublishActiveDisplay(monitor, hdrState);
    }
};

void VerifyOwnedTracePresents(Device& device, IDXGISwapChain* chain, SwapChainRecord& record)
{
    TraceOwnedPresentScope scope(device);
    const bool descriptor = record.developerHdrDescriptor;
    record.developerHdrDescriptor = false;
    VerifyTracePresentForwarding(chain, DXGI_PRESENT_DO_NOT_WAIT);
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    for (unsigned frame = 0; frame < 8; ++frame)
        VerifyTracePresentForwarding(chain, DXGI_PRESENT_DO_NOT_WAIT);
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=24 stop=2 dropped=0"));
    unsigned eventIndex{};
    for (const auto& line : g_traceMessages)
    {
        if (TraceField(line, 0).empty()) continue;
        FO4HDR_WARP_CHECK(TraceField(line, 2) == std::to_string(eventIndex / 3 + 1));
        ++eventIndex;
    }
    FO4HDR_WARP_CHECK(eventIndex == 24);
    record.developerHdrDescriptor = descriptor;
}

void VerifyTraceFrames(Device& device)
{
    PresentationWindow window;
    auto chain = CreatePresentationChain(device, window.handle, 37, 19);
    auto& record = g_swapChains.front();
    const auto next = record.nextPresent;
    const auto owned = record.ownsRendererClaim;
    FO4HDR_WARP_CHECK(record.identity.Publish(chain.Get(), 92));
    record.nextPresent = &TraceProbePresent;
    record.ownsRendererClaim = false;
    VerifyTracePresentForwarding(chain.Get(), DXGI_PRESENT_DO_NOT_WAIT);
    record.ownsRendererClaim = true;
    FO4HDR_WARP_CHECK(BeginFrameTrace());
    for (unsigned frame = 1; frame <= 8; ++frame)
    {
        VerifyTracePresentForwarding(chain.Get(), DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT);
        TraceFramePresent(chain.Get(), FrameTraceKind::nativePresent, {2, DXGI_PRESENT_DO_NOT_WAIT});
        TraceFramePresent(chain.Get(), FrameTraceKind::nextPresent, {2, DXGI_PRESENT_DO_NOT_WAIT});
        TraceFramePresent(chain.Get(), FrameTraceKind::presentResult,
            {2, DXGI_PRESENT_DO_NOT_WAIT, DXGI_STATUS_OCCLUDED});
    }
    TraceFramePresent(chain.Get(), FrameTraceKind::presentResult, {2, 0});
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=40 stop=2 dropped=0"));
    unsigned eventIndex{};
    for (const auto& line : g_traceMessages)
    {
        if (TraceField(line, 0).empty()) continue;
        FO4HDR_WARP_CHECK(TraceField(line, 2) == std::to_string(eventIndex / 5 + 1));
        ++eventIndex;
    }
    FO4HDR_WARP_CHECK(eventIndex == 40);
    FO4HDR_WARP_CHECK(TraceContains(std::format("args=2/{}/{}/0",
        DXGI_PRESENT_DO_NOT_WAIT, static_cast<unsigned>(DXGI_STATUS_OCCLUDED))));
    VerifyOwnedTracePresents(device, chain.Get(), record);
    record.nextPresent = next;
    record.ownsRendererClaim = owned;
    FO4HDR_WARP_CHECK(record.identity.Retire(chain.Get(), 92));

    FO4HDR_WARP_CHECK(BeginFrameTrace());
    ComPtr<ID3D11Texture2D> backbuffer;
    CheckHresult(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "Get trace lifetime backbuffer");
    TraceFrameCopy(device.context.Get(), {}, {FrameTraceKind::copy, backbuffer.Get(), backbuffer.Get()});
    TraceFramePresent(chain.Get(), FrameTraceKind::nativePresent, {1, 0});
    RetireFrameTrace();
    backbuffer.Reset();
    CheckHresult(chain->ResizeBuffers(2, 41, 23, DXGI_FORMAT_UNKNOWN, 0),
        "Resize with frozen trace retaining only numeric resource identities");
    g_traceMessages.clear();
    ReportFrameTrace(&CaptureTraceMessage);
    FO4HDR_WARP_CHECK(TraceContains("events=2 stop=4 dropped=0"));
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[1], 2) == "1");
    FO4HDR_WARP_CHECK(TraceField(g_traceMessages[2], 2) == "1");
}

} // namespace

void VerifyFrameTrace(Device& device)
{
    const bool requested = FrameTraceRequested();
    VerifyNonProductionMutationHooks(device);
    VerifyTraceCallbacks(device);
    FO4HDR_WARP_CHECK(InitializeFrameTrace(true));
    VerifySharedUiTrace(device);
    VerifyArmedTraceRollingAndTrigger();
    VerifyArmedTraceCancelAndReportRetry();
    VerifyTraceHotkeyRearmsAfterCancellation();
    VerifyTraceHotkeyModeAndThreadMigration();
    VerifyArmedTraceRetirement(device);
    VerifyArmedTraceLifecycleReset();
    VerifyArmedTraceFailedPresent();
    VerifyContendedBlackTraceRetainsPresentResult();
    VerifyArmedTraceBoundedHistory();
    VerifyArmedTraceCancelRace();
    VerifyTraceReplacementAttribution(device);
    VerifyTraceFrames(device);
    VerifyAutomaticTraceCompletion();
    VerifyAutomaticTraceCancelAndRestart();
    VerifyAutomaticTraceUnregister();
    VerifyAutomaticTraceThrowingSink();
    (void)InitializeFrameTrace(requested);
}

} // namespace fo4hdr::tests::gpu
