// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FrameTrace.hpp"

#include "FrameTraceBuffer.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "ShaderRegistry.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"
#include "FO4Isotope/ui/HdrSettingsInput.hpp"

#include <wrl/client.h>
#include <atomic>
#include <bit>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::size_t kTraceCapacity = 65536;
constexpr std::uint32_t kTracePresents = 8;

struct TraceResource
{
    std::uintptr_t identity{};
    std::uint32_t width{}, height{}, format{}, samples{}, viewFormat{}, mip{}, slice{};
    std::uint32_t bindFlags{}, mipLevels{}, arraySize{};
};

struct TraceEvent
{
    std::uint64_t sequence{}, ticks{}, frame{}, resourceGeneration{}, resetEpoch{}, contextGeneration{};
    std::uintptr_t context{}, shader{};
    std::uint32_t thread{}, shaderCrc{}, pass{}, inputSlot{}, flags{};
    FrameTraceKind kind{};
    FrameTraceArguments arguments{};
    ArmorUiScopeToken scope{};
    std::array<TraceResource, 3> resources{};
};

using TraceBuffer = FrameTraceBuffer<TraceEvent, kTraceCapacity>;
std::unique_ptr<TraceBuffer> g_trace;
std::atomic<bool> g_requested{};
std::atomic<bool> g_storageFailure{};
std::uint64_t g_traceFrame{1};
std::uint64_t g_traceSession{};
std::atomic<std::uint64_t> g_traceResetEpoch{};
enum class ArmedTracePhase { ordinary, warmup, triggered, cancelled };
std::atomic<ArmedTracePhase> g_armedPhase{};
std::atomic<bool> g_armed{}, g_incomplete{}, g_partialFrame{};
std::atomic<bool> g_triggerPresentRecorded{};
std::atomic<GraphicsDiagnosticSink> g_autoReportSink{};
std::atomic<bool> g_autoReportPending{};
thread_local bool g_ownedCall{};

void ReportFrameTraceForEpoch(GraphicsDiagnosticSink sink, std::uint64_t epoch);

DWORD WINAPI AutoReportFrameTrace(void* captureEpoch) noexcept
{
    auto sink = g_autoReportSink.load(std::memory_order_acquire);
    try
    {
        if (sink)
            ReportFrameTraceForEpoch(sink, reinterpret_cast<std::uintptr_t>(captureEpoch));
    }
    catch (...)
    {
        (void)g_autoReportSink.compare_exchange_strong(sink, nullptr, std::memory_order_acq_rel);
        OutputDebugStringA("FO4Isotope: automatic frame trace reporting failed.\n");
    }
    g_autoReportPending.store(false, std::memory_order_release);
    return 0;
}

void QueueFrameTraceReport() noexcept
{
    if (!FrameTraceRequested() || !g_autoReportSink.load(std::memory_order_acquire)) return;
    const auto captureEpoch = g_trace->Epoch();
    const auto state = g_trace->State();
    if (state == FrameTraceStop::idle || state == FrameTraceStop::recording) return;
    bool expected{};
    if (!g_autoReportPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    const auto epoch = reinterpret_cast<void*>(static_cast<std::uintptr_t>(captureEpoch));
    if (!QueueUserWorkItem(&AutoReportFrameTrace, epoch, WT_EXECUTEDEFAULT))
        g_autoReportPending.store(false, std::memory_order_release);
}

bool Triggered() noexcept
{
    return g_armedPhase.load() == ArmedTracePhase::triggered;
}

bool BeginTrace(bool armed) noexcept
{
    if (!FrameTraceRequested()) return false;
    return g_trace->Begin([=]() noexcept {
        g_traceFrame = 1;
        g_traceResetEpoch.store(ReadGraphicsLifecycle().resetEpoch);
        g_armed.store(armed);
        g_armedPhase.store(armed ? ArmedTracePhase::warmup : ArmedTracePhase::ordinary);
        g_incomplete.store(false);
        g_partialFrame.store(false);
        g_triggerPresentRecorded.store(false);
        ++g_traceSession;
    }, armed);
}

void FillEvent(TraceEvent& event, std::uint64_t sequence, FrameTraceKind kind) noexcept
{
    event = {};
    event.sequence = sequence;
    event.kind = kind;
    event.thread = GetCurrentThreadId();
    LARGE_INTEGER ticks{};
    (void)QueryPerformanceCounter(&ticks);
    event.ticks = static_cast<std::uint64_t>(ticks.QuadPart);
    event.frame = g_traceFrame;
    event.resourceGeneration = g_evidence.resourceGeneration.load(std::memory_order_acquire);
    const auto lifecycle = ReadGraphicsLifecycle();
    event.resetEpoch = lifecycle.resetEpoch;
    if (lifecycle.resetEpoch != g_traceResetEpoch.load())
    {
        if (g_armed.load())
        {
            g_partialFrame.store(true);
            g_traceResetEpoch.store(lifecycle.resetEpoch);
        }
        else g_trace->Stop(FrameTraceStop::retired);
    }
    event.flags = (lifecycle.gameplayReady ? 1U : 0U) | (lifecycle.presentationBlocked ? 2U : 0U);
    if (g_ownedCall) event.flags |= 32U;
    event.scope = CurrentArmorUiScope();
}

template <class Writer, class Before, class After>
bool RecordEvent(FrameTraceKind kind, Writer&& writer, Before&& before, After&& after) noexcept
{
    static_assert(std::is_nothrow_invocable_v<Writer, TraceEvent&>);
    if (!FrameTraceRequested()) return false;
    const auto epoch = g_trace->Epoch();
    bool recorded{};
    (void)g_trace->WithCapture(epoch, [&]() noexcept {
        std::forward<Before>(before)();
        recorded = g_trace->Record(epoch, [&](TraceEvent& event, std::uint64_t sequence) noexcept {
            FillEvent(event, sequence, kind);
            std::forward<Writer>(writer)(event);
        });
        std::forward<After>(after)(recorded);
    });
    return recorded;
}

template <class Writer>
bool RecordEvent(FrameTraceKind kind, Writer&& writer) noexcept
{
    return RecordEvent(kind, std::forward<Writer>(writer), []() noexcept {},
        [](bool) noexcept {});
}

void DescribeResource(ID3D11Resource* resource, TraceResource& result) noexcept
{
    result.identity = reinterpret_cast<std::uintptr_t>(resource);
    if (!resource) return;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    result.width = description.Width;
    result.height = description.Height;
    result.format = description.Format;
    result.samples = description.SampleDesc.Count;
    result.bindFlags = description.BindFlags;
    result.mipLevels = description.MipLevels;
    result.arraySize = description.ArraySize;
}

template <class View, class Description>
bool DescribeView(View* view, TraceResource& result, Description& description) noexcept
{
    if (!view) return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    DescribeResource(resource.Get(), result);
    view->GetDesc(&description);
    result.viewFormat = description.Format;
    return true;
}

void DescribeTarget(ID3D11RenderTargetView* target, TraceResource& result) noexcept
{
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    if (!DescribeView(target, result, description)) return;
    if (description.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
        result.mip = description.Texture2D.MipSlice;
    if (description.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
    {
        result.mip = description.Texture2DArray.MipSlice;
        result.slice = description.Texture2DArray.FirstArraySlice;
    }
}

void DescribeInput(ID3D11ShaderResourceView* view, TraceResource& result) noexcept
{
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    if (!DescribeView(view, result, description)) return;
    if (description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
        result.mip = description.Texture2D.MostDetailedMip;
    if (description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
    {
        result.mip = description.Texture2DArray.MostDetailedMip;
        result.slice = description.Texture2DArray.FirstArraySlice;
    }
}

const RenoDxShaderDescriptor* DescribeShader(ID3D11PixelShader* shader,
    const DrawObservation& observation) noexcept
{
    if (const auto* known = FindShader(shader)) return known;
    if (!observation.currentPassKnown || !observation.currentReplacementActive) return nullptr;
    if (!observation.expectedBoundShader || shader != observation.expectedBoundShader) return nullptr;
    return FindShaderDescriptor(observation.currentPass);
}

void DescribeDraw(ID3D11DeviceContext* context, const DrawObservation& observation,
    TraceEvent& event) noexcept
{
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    event.shader = reinterpret_cast<std::uintptr_t>(shader.Get());
    if (shader)
    {
        UINT bytes = sizeof(event.shaderCrc);
        (void)shader->GetPrivateData(kFrameTraceShaderCrc, &bytes, &event.shaderCrc);
        if (const auto* known = DescribeShader(shader.Get(), observation))
        {
            event.pass = static_cast<std::uint32_t>(known->pass) + 1;
            event.inputSlot = known->sceneInputSlot;
        }
        if (IsLateGameUiShader(shader.Get())) event.flags |= 4U;
        if (IsLateGameUiBackdropShader(shader.Get())) event.flags |= 8U;
    }
    ID3D11RenderTargetView* targets[2]{};
    context->OMGetRenderTargets(2, targets, nullptr);
    for (std::size_t index = 0; index < 2; ++index)
    {
        ComPtr<ID3D11RenderTargetView> target;
        target.Attach(targets[index]);
        DescribeTarget(target.Get(), event.resources[index]);
    }
    ComPtr<ID3D11ShaderResourceView> input;
    context->PSGetShaderResources(event.inputSlot, 1, &input);
    DescribeInput(input.Get(), event.resources[2]);
}

void FillContext(TraceEvent& event, ID3D11DeviceContext* context,
    const DrawObservation& observation) noexcept
{
    event.context = reinterpret_cast<std::uintptr_t>(context);
    event.contextGeneration = observation.generation;
    if (!observation.immediate) event.flags |= 16U;
    if (!observation.computeQueries.CanDispatch()) event.flags |= 128U;
}

void DescribeBackbuffer(IDXGISwapChain* chain, TraceResource& result) noexcept
{
    ComPtr<ID3D11Texture2D> backbuffer;
    if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        DescribeResource(backbuffer.Get(), result);
}

std::array<std::uint32_t, 2> ReadTraceNativeMenu() noexcept
{
    const auto generation = HdrSettingsNativeMenuGeneration();
    const auto state = ReadHdrSettingsNativeMenuState();
    const auto currentGeneration = HdrSettingsNativeMenuGeneration();
    const auto coherentState = generation == currentGeneration ?
        state : HdrSettingsNativeMenuState::unknown;
    return {currentGeneration, static_cast<std::uint32_t>(coherentState)};
}

FrameTraceArguments SharedUiArguments(const FrameTraceSharedUiSnapshot& snapshot) noexcept
{
    const std::array states{snapshot.seeded, snapshot.closed, snapshot.nativeWritten,
        snapshot.rejected, snapshot.outputWritten, snapshot.assumed};
    std::uint32_t flags{};
    for (std::size_t index = 0; index < states.size(); ++index)
        flags |= static_cast<std::uint32_t>(states[index]) << index;
    const auto menu = ReadTraceNativeMenu();
    return {static_cast<std::uint32_t>(snapshot.decision), snapshot.rejection, flags,
        std::bit_cast<std::uint32_t>(snapshot.frozenWhite),
        static_cast<std::uint32_t>(snapshot.contentRevision),
        static_cast<std::uint32_t>(snapshot.contentRevision >> 32),
        static_cast<std::uint32_t>(snapshot.generation),
        static_cast<std::uint32_t>(snapshot.generation >> 32),
        static_cast<std::uint32_t>(snapshot.frame), static_cast<std::uint32_t>(snapshot.frame >> 32),
        menu[0], menu[1]};
}

void AdvanceTraceFrame(FrameTraceKind kind, UINT flags) noexcept
{
    if (kind != FrameTraceKind::presentResult || (flags & DXGI_PRESENT_TEST) != 0) return;
    if (++g_traceFrame > kTracePresents && !g_armed.load()) g_trace->Stop(FrameTraceStop::complete);
}

void FinishArmedFrame(bool recorded, HRESULT result) noexcept
{
    if (FAILED(result)) g_incomplete.store(true);
    if (Triggered())
    {
        if (!recorded) g_incomplete.store(true);
        g_triggerPresentRecorded.store(recorded);
        g_trace->Stop(FrameTraceStop::complete);
    }
    else if (FAILED(result))
    {
        g_trace->Stop(FrameTraceStop::retired);
    }
    else if (recorded) g_partialFrame.store(false);
}

void ApplyTraceHotkey(bool arm) noexcept
{
    if (!arm) (void)BeginFrameTrace();
    else if (g_trace->Recording() && g_armedPhase.load() == ArmedTracePhase::warmup) CancelFrameTrace();
    else (void)ArmFrameTrace();
}

bool TraceIncomplete(FrameTraceStop reason, std::uint64_t dropped) noexcept
{
    if (g_incomplete.load() || g_partialFrame.load()) return true;
    if (dropped != 0) return true;
    if (g_trace->Overwritten() != 0) return true;
    if (reason == FrameTraceStop::capacity) return true;
    return Triggered() && !g_triggerPresentRecorded.load();
}

std::string ResourceText(const TraceResource& resource)
{
    return std::format("{:x}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}", resource.identity, resource.width,
        resource.height, resource.format, resource.samples, resource.viewFormat, resource.mip, resource.slice,
        resource.bindFlags, resource.mipLevels, resource.arraySize);
}

std::string ScopeName(const ArmorUiScopeToken& scope)
{
    std::string result;
    for (const char value : scope.rendererName)
    {
        if (!value) break;
        result += value >= ' ' && value <= '~' && value != ',' ? value : '?';
    }
    return result;
}

void ReportEvent(GraphicsDiagnosticSink sink, const TraceEvent& event)
{
    const auto& args = event.arguments;
    sink(std::format("frame-trace event={},{},{},{},{},{},{},{},{:x},{:x},{:08x},{},{},{} "
        "args={}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{} r0={} r1={} r2={} "
        "scope={}/{}/{}/{}/{}/{}/{}/{} name={}",
        event.sequence, event.ticks, event.frame, static_cast<unsigned>(event.kind), event.thread,
        event.resourceGeneration, event.resetEpoch, event.contextGeneration, event.context, event.shader,
        event.shaderCrc, event.pass, event.inputSlot, event.flags,
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11],
        ResourceText(event.resources[0]), ResourceText(event.resources[1]), ResourceText(event.resources[2]),
        static_cast<unsigned>(event.scope.owner), event.scope.threadId, event.scope.serial,
        event.scope.requestGeneration, event.scope.targetArgument, event.scope.depth,
        event.scope.postAa, event.scope.hasRoot, ScopeName(event.scope)));
}

} // namespace

ScopedFrameTraceOwnedCall::ScopedFrameTraceOwnedCall() noexcept : previous_(g_ownedCall)
{
    g_ownedCall = true;
}

ScopedFrameTraceOwnedCall::~ScopedFrameTraceOwnedCall()
{
    g_ownedCall = previous_;
}

bool InitializeFrameTrace(bool requested) noexcept
{
    if (requested && !g_trace) g_trace.reset(new (std::nothrow) TraceBuffer{});
    const bool enabled = requested && g_trace != nullptr;
    g_storageFailure.store(requested && !enabled, std::memory_order_release);
    g_requested.store(enabled, std::memory_order_release);
    return enabled;
}

bool FrameTraceRequested() noexcept
{
    return g_requested.load(std::memory_order_acquire);
}

bool BeginFrameTrace() noexcept
{
    return BeginTrace(false);
}

bool ArmFrameTrace() noexcept
{
    if (!FrameTraceRequested() || !g_configuration.productionRequested) return false;
    return BeginTrace(true);
}

void CancelFrameTrace() noexcept
{
    if (!FrameTraceRequested()) return;
    (void)g_trace->WithCapture(g_trace->Epoch(), []() noexcept {
        auto expected = ArmedTracePhase::warmup;
        if (!g_armedPhase.compare_exchange_strong(expected, ArmedTracePhase::cancelled)) return;
        g_trace->Stop(FrameTraceStop::retired);
    });
    QueueFrameTraceReport();
}

void RetireFrameTrace() noexcept
{
    if (!FrameTraceRequested()) return;
    (void)g_trace->WithCapture(g_trace->Epoch(), []() noexcept {
        if (g_armed.load())
        {
            g_partialFrame.store(true);
            if (Triggered()) g_incomplete.store(true);
        }
        else g_trace->Stop(FrameTraceStop::retired);
    });
    QueueFrameTraceReport();
}

void TraceFrameDraw(ID3D11DeviceContext* context, const DrawObservation& observation,
    FrameTraceKind kind, const FrameTraceArguments& arguments) noexcept
{
    RecordEvent(kind, [&](TraceEvent& event) noexcept {
        FillContext(event, context, observation);
        event.arguments = arguments;
        if (observation.currentReplacementActive) event.flags |= 64U;
        DescribeDraw(context, observation, event);
    });
}

void TraceFrameCopy(ID3D11DeviceContext* context, const DrawObservation& observation,
    const FrameTraceCopyArguments& arguments) noexcept
{
    RecordEvent(arguments.kind, [&](TraceEvent& event) noexcept {
        FillContext(event, context, observation);
        event.arguments = arguments.region;
        DescribeResource(arguments.destination, event.resources[0]);
        DescribeResource(arguments.source, event.resources[1]);
    });
}

void TraceFrameClear(ID3D11DeviceContext* context, const DrawObservation& observation,
    ID3D11RenderTargetView* target, const FLOAT* color) noexcept
{
    RecordEvent(FrameTraceKind::clear, [&](TraceEvent& event) noexcept {
        FillContext(event, context, observation);
        DescribeTarget(target, event.resources[0]);
        if (color) for (std::size_t index = 0; index < 4; ++index)
            event.arguments[index] = std::bit_cast<std::uint32_t>(color[index]);
    });
}

void TraceFrameCommandList(ID3D11DeviceContext* context, const DrawObservation& observation,
    ID3D11CommandList* commands, BOOL restoreState) noexcept
{
    RecordEvent(FrameTraceKind::commandList, [&](TraceEvent& event) noexcept {
        FillContext(event, context, observation);
        event.resources[0].identity = reinterpret_cast<std::uintptr_t>(commands);
        event.arguments[0] = static_cast<std::uint32_t>(restoreState);
    });
}

void TraceNativeBoundary(const ArmorUiScopeToken& scope, bool entering) noexcept
{
    RecordEvent(entering ? FrameTraceKind::nativeEnter : FrameTraceKind::nativeExit,
        [&](TraceEvent& event) noexcept {
            event.scope = scope;
        });
}

void TraceFramePresent(IDXGISwapChain* chain, FrameTraceKind kind,
    const FrameTracePresentArguments& arguments) noexcept
{
    RecordEvent(kind, [&](TraceEvent& event) noexcept {
        event.context = reinterpret_cast<std::uintptr_t>(chain);
        event.arguments = {arguments.interval, arguments.flags, static_cast<std::uint32_t>(arguments.result)};
        if (kind == FrameTraceKind::nativePresent)
            DescribeBackbuffer(chain, event.resources[0]);
        AdvanceTraceFrame(kind, arguments.flags);
    }, []() noexcept {}, [&](bool accepted) noexcept {
        if (!g_armed.load() || kind != FrameTraceKind::presentResult) return;
        if ((arguments.flags & DXGI_PRESENT_TEST) != 0) return;
        FinishArmedFrame(accepted, arguments.result);
    });
    if (kind == FrameTraceKind::presentResult) QueueFrameTraceReport();
}

void TraceSharedUiPresentation(const FrameTraceSharedUiSnapshot& snapshot) noexcept
{
    RecordEvent(FrameTraceKind::sharedUiPresentation, [&](TraceEvent& event) noexcept {
        event.context = reinterpret_cast<std::uintptr_t>(snapshot.context);
        event.contextGeneration = snapshot.contextGeneration;
        event.arguments = SharedUiArguments(snapshot);
        DescribeInput(snapshot.selectedScene, event.resources[0]);
        DescribeResource(snapshot.publicBuffer, event.resources[1]);
        DescribeResource(snapshot.outputBuffer, event.resources[2]);
    }, [&]() noexcept {
        // The decision survives a contended event writer so the failure cannot silently rearm.
        if (snapshot.decision != FrameTraceSharedUiDecision::black) return;
        auto expected = ArmedTracePhase::warmup;
        (void)g_armedPhase.compare_exchange_strong(expected, ArmedTracePhase::triggered);
    }, [](bool recorded) noexcept {
        if (!Triggered() || recorded) return;
        g_incomplete.store(true);
    });
}

void PollFrameTraceHotkey(IDXGISwapChain* chain) noexcept
{
    if (!FrameTraceRequested()) return;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
    DXGI_SWAP_CHAIN_DESC description{};
    if (SUCCEEDED(chain->GetDesc(&description)) && description.OutputWindow == GetForegroundWindow())
        ProcessFrameTraceHotkey(down, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    else (void)g_trace->HotkeyPressed(down);
}

void ProcessFrameTraceHotkey(bool down, bool arm) noexcept
{
    if (!FrameTraceRequested()) return;
    if (g_trace->HotkeyPressed(down)) ApplyTraceHotkey(arm);
}

void SetFrameTraceAutoReportSink(GraphicsDiagnosticSink sink) noexcept
{
    g_autoReportSink.store(sink, std::memory_order_release);
    if (sink) QueueFrameTraceReport();
}

bool FrameTraceAutoReportPending() noexcept
{
    return g_autoReportPending.load(std::memory_order_acquire);
}

namespace {

void ReportFrameTraceForEpoch(GraphicsDiagnosticSink sink, std::uint64_t epoch)
{
    if (!sink) return;
    if (g_storageFailure.exchange(false, std::memory_order_acq_rel))
        sink("frame-trace unavailable: fixed capture storage allocation failed");
    if (!FrameTraceRequested()) return;
    (void)g_trace->Report([&](std::span<const TraceEvent> events, FrameTraceStop reason, std::uint64_t dropped) {
        LARGE_INTEGER frequency{};
        (void)QueryPerformanceFrequency(&frequency);
        sink(std::format("frame-trace begin version=2 session={} events={} stop={} dropped={} qpc_frequency={} "
            "coverage=submission-only rt_slots=0,1 srv=known-scene-slot-or-0 indirect-resolve-uav-writes=unobserved "
            "command-list-playback=gap resource-addresses=non-owning-reusable "
            "resource-fields=address/width/height/format/samples/view-format/mip/slice/bind-flags/mip-levels/array-size "
            "context-flags=query-injection-blocked:128 "
            "shared-ui-kind=13 shared-ui-context-generation=content shared-ui-resources=selected/public/output "
            "shared-ui-decisions=hdr:0,startup-sdr:1,black:2,sdr-content-hdr:3 "
            "shared-ui-args=decision/rejection/content-flags/white-f32-bits/revision-lo/revision-hi/"
            "generation-lo/generation-hi/frame-lo/frame-hi/menu-generation/menu-state "
            "shared-ui-content-flags=seeded:1,closed:2,native-written:4,rejected:8,output-written:16,assumed:32 "
            "mode={} trigger={} incomplete={} cancelled={} overwritten={} first_sequence={}",
            g_traceSession, events.size(), static_cast<unsigned>(reason), dropped, frequency.QuadPart,
            g_armed.load() ? "first-black" : "eight-frames", Triggered() ? "black" : "none",
            TraceIncomplete(reason, dropped), g_armedPhase.load() == ArmedTracePhase::cancelled,
            g_trace->Overwritten(), events.empty() ? 0 : events.front().sequence));
        for (const auto& event : events) ReportEvent(sink, event);
        sink(std::format("frame-trace end session={}", g_traceSession));
    }, epoch);
}

} // namespace

void ReportFrameTrace(GraphicsDiagnosticSink sink)
{
    ReportFrameTraceForEpoch(sink, 0);
}

} // namespace fo4hdr
