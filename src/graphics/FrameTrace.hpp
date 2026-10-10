// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "DrawObservation.hpp"
#include "FO4Isotope/graphics/HdrRenderer.hpp"
#include "FO4Isotope/output/ArmorUiComposition.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <cstdint>

namespace fo4hdr {

enum class FrameTraceKind : std::uint8_t
{
    draw, drawIndexed, drawInstanced, drawIndexedInstanced,
    copy, copyRegion, clear, commandList,
    nativeEnter, nativeExit, nativePresent, nextPresent, presentResult, sharedUiPresentation
};

using FrameTraceArguments = std::array<std::uint32_t, 12>;

struct FrameTraceCopyArguments
{
    FrameTraceKind kind{FrameTraceKind::copy};
    ID3D11Resource* destination{};
    ID3D11Resource* source{};
    FrameTraceArguments region{};
};

struct FrameTracePresentArguments
{
    UINT interval{};
    UINT flags{};
    HRESULT result{S_OK};
};

enum class FrameTraceSharedUiDecision : std::uint32_t
{
    hdr, startupSdr, black, hdrSdrContent, hdrPublicContent
};

struct FrameTraceSharedUiSnapshot
{
    std::uint64_t contentRevision{}, generation{}, contextGeneration{}, frame{};
    float frozenWhite{};
    bool seeded{}, closed{}, nativeWritten{}, rejected{}, outputWritten{}, assumed{};
    std::uint32_t rejection{};
    FrameTraceSharedUiDecision decision{FrameTraceSharedUiDecision::black};
    ID3D11DeviceContext* context{};
    ID3D11ShaderResourceView* selectedScene{};
    ID3D11Resource* publicBuffer{};
    ID3D11Resource* outputBuffer{};
};

class ScopedFrameTraceOwnedCall
{
public:
    ScopedFrameTraceOwnedCall() noexcept;
    ~ScopedFrameTraceOwnedCall();
    ScopedFrameTraceOwnedCall(const ScopedFrameTraceOwnedCall&) = delete;
    ScopedFrameTraceOwnedCall& operator=(const ScopedFrameTraceOwnedCall&) = delete;

private:
    bool previous_{};
};

// Storage is allocated only at diagnostic initialization; callbacks retain no COM objects.
[[nodiscard]] bool InitializeFrameTrace(bool requested) noexcept;
[[nodiscard]] bool FrameTraceRequested() noexcept;
// The owned Present thread starts captures; stopped captures report outside graphics callbacks.
[[nodiscard]] bool BeginFrameTrace() noexcept;
void RetireFrameTrace() noexcept;
void ReportFrameTrace(GraphicsDiagnosticSink sink);

void TraceFrameDraw(ID3D11DeviceContext* context, const DrawObservation& observation,
    FrameTraceKind kind, const FrameTraceArguments& arguments) noexcept;
void TraceFrameCopy(ID3D11DeviceContext* context, const DrawObservation& observation,
    const FrameTraceCopyArguments& arguments) noexcept;
void TraceFrameClear(ID3D11DeviceContext* context, const DrawObservation& observation,
    ID3D11RenderTargetView* target, const FLOAT* color) noexcept;
void TraceFrameCommandList(ID3D11DeviceContext* context, const DrawObservation& observation,
    ID3D11CommandList* commands, BOOL restoreState) noexcept;
void TraceNativeBoundary(const ArmorUiScopeToken& scope, bool entering) noexcept;
void TraceFramePresent(IDXGISwapChain* chain, FrameTraceKind kind,
    const FrameTracePresentArguments& arguments) noexcept;
void TraceSharedUiPresentation(const FrameTraceSharedUiSnapshot&) noexcept;
void PollFrameTraceHotkey(IDXGISwapChain* chain) noexcept;

} // namespace fo4hdr
