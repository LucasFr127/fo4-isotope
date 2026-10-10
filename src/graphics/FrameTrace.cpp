// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "FrameTrace.hpp"

#include "FrameTraceBuffer.hpp"
#include "GraphicsDiagnostics.hpp"
#include "ShaderRegistry.hpp"
#include "FO4Isotope/output/HdrActivationPolicy.hpp"

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
thread_local bool g_ownedCall{};

bool BeginTrace() noexcept
{
    if (!FrameTraceRequested()) return false;
    return g_trace->Begin([]() noexcept {
        g_traceFrame = 1;
        g_traceResetEpoch.store(ReadGraphicsLifecycle().resetEpoch);
        ++g_traceSession;
    });
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
        g_trace->Stop(FrameTraceStop::retired);
    event.flags = (lifecycle.gameplayReady ? 1U : 0U) | (lifecycle.presentationBlocked ? 2U : 0U);
    if (g_ownedCall) event.flags |= 32U;
    event.scope = CurrentArmorUiScope();
}

template <class Writer>
bool RecordEvent(FrameTraceKind kind, Writer&& writer) noexcept
{
    static_assert(std::is_nothrow_invocable_v<Writer, TraceEvent&>);
    if (!FrameTraceRequested()) return false;
    const auto epoch = g_trace->Epoch();
    return g_trace->Record(epoch, [&](TraceEvent& event, std::uint64_t sequence) noexcept {
        FillEvent(event, sequence, kind);
        std::forward<Writer>(writer)(event);
    });
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
    if (chain && SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        DescribeResource(backbuffer.Get(), result);
}

FrameTraceArguments SharedUiArguments(const FrameTraceSharedUiSnapshot& snapshot) noexcept
{
    const std::array states{snapshot.seeded, snapshot.closed, snapshot.nativeWritten,
        snapshot.rejected, snapshot.outputWritten, snapshot.assumed};
    std::uint32_t flags{};
    for (std::size_t index = 0; index < states.size(); ++index)
        flags |= static_cast<std::uint32_t>(states[index]) << index;
    return {static_cast<std::uint32_t>(snapshot.decision), snapshot.rejection, flags,
        std::bit_cast<std::uint32_t>(snapshot.frozenWhite),
        static_cast<std::uint32_t>(snapshot.contentRevision),
        static_cast<std::uint32_t>(snapshot.contentRevision >> 32),
        static_cast<std::uint32_t>(snapshot.generation),
        static_cast<std::uint32_t>(snapshot.generation >> 32),
        static_cast<std::uint32_t>(snapshot.frame), static_cast<std::uint32_t>(snapshot.frame >> 32)};
}

void AdvanceTraceFrame(FrameTraceKind kind, UINT flags) noexcept
{
    if (kind != FrameTraceKind::presentResult) return;
    if ((flags & DXGI_PRESENT_TEST) != 0) return;
    if (++g_traceFrame > kTracePresents) g_trace->Stop(FrameTraceStop::complete);
}

std::string ResourceText(const TraceResource& resource)
{
    return std::format("{:x}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}", resource.identity, resource.width,
        resource.height, resource.format, resource.samples, resource.viewFormat, resource.mip, resource.slice,
        resource.bindFlags, resource.mipLevels, resource.arraySize);
}

void ReportEvent(GraphicsDiagnosticSink sink, const TraceEvent& event)
{
    const auto& args = event.arguments;
    sink(std::format("frame-trace event={},{},{},{},{},{},{},{},{:x},{:x},{:08x},{},{},{} "
        "args={}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{} r0={} r1={} r2={} "
        "scope={}/{}/{}/{}/{}/{}/{}/{}",
        event.sequence, event.ticks, event.frame, static_cast<unsigned>(event.kind), event.thread,
        event.resourceGeneration, event.resetEpoch, event.contextGeneration, event.context, event.shader,
        event.shaderCrc, event.pass, event.inputSlot, event.flags,
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11],
        ResourceText(event.resources[0]), ResourceText(event.resources[1]), ResourceText(event.resources[2]),
        static_cast<unsigned>(event.scope.owner), event.scope.threadId, event.scope.serial,
        event.scope.requestGeneration, event.scope.targetArgument, event.scope.depth,
        event.scope.postAa, event.scope.hasRoot));
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
    return BeginTrace();
}

void RetireFrameTrace() noexcept
{
    if (FrameTraceRequested()) g_trace->Stop(FrameTraceStop::retired);
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
        if (kind == FrameTraceKind::nativePresent) DescribeBackbuffer(chain, event.resources[0]);
        AdvanceTraceFrame(kind, arguments.flags);
    });
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
    });
}

void PollFrameTraceHotkey(IDXGISwapChain* chain) noexcept
{
    if (!FrameTraceRequested() || !chain) return;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
    DXGI_SWAP_CHAIN_DESC description{};
    const bool pressed = g_trace->HotkeyPressed(down);
    if (!pressed) return;
    if (FAILED(chain->GetDesc(&description))) return;
    if (description.OutputWindow != GetForegroundWindow()) return;
    (void)BeginFrameTrace();
}

void ReportFrameTrace(GraphicsDiagnosticSink sink)
{
    if (!sink) return;
    if (g_storageFailure.exchange(false, std::memory_order_acq_rel))
        sink("frame-trace unavailable: fixed capture storage allocation failed");
    if (!FrameTraceRequested()) return;
    (void)g_trace->Report([&](std::span<const TraceEvent> events, FrameTraceStop reason, std::uint64_t dropped) {
        LARGE_INTEGER frequency{};
        (void)QueryPerformanceFrequency(&frequency);
        sink(std::format("frame-trace begin version=1 session={} events={} stop={} dropped={} qpc_frequency={} "
            "coverage=submission-only rt_slots=0,1 srv=known-scene-slot-or-0 indirect-resolve-uav-writes=unobserved "
            "command-list-playback=gap resource-addresses=non-owning-reusable mode=eight-frames",
            g_traceSession, events.size(), static_cast<unsigned>(reason), dropped, frequency.QuadPart));
        for (const auto& event : events) ReportEvent(sink, event);
        sink(std::format("frame-trace end session={}", g_traceSession));
    });
}

} // namespace fo4hdr
