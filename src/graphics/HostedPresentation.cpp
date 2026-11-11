// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "HostedPresentation.hpp"

#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "RendererLifetime.hpp"
#include "resources/ReplacementShaders.hpp"
#include <Windows.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

std::atomic<bool> g_hostedHdrActive{};
HostedPresentationState g_hostedState;

namespace {

// The first loaded module's export of that name, or null.
[[nodiscard]] FARPROC FindLoadedModuleExport(const char* symbol) noexcept
{
    std::array<HMODULE, 1024> modules{};
    DWORD needed{};
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
            static_cast<DWORD>(sizeof(modules)), &needed))
        return nullptr;
    const auto count = std::min<std::size_t>(needed / sizeof(HMODULE), modules.size());
    for (std::size_t index = 0; index < count; ++index)
    {
        if (modules[index] == nullptr) continue;
        if (const auto address = GetProcAddress(modules[index], symbol)) return address;
    }
    return nullptr;
}

// The host's HDR container reports R8G8B8A8_UNORM while its back buffer is FP16.
struct HostedChainFormats
{
    bool chainRgba8Unorm{};
    bool backBufferFp16{};
};

[[nodiscard]] HostedChainFormats ReadHostedChainFormats(IDXGISwapChain* swapChain) noexcept
{
    DXGI_SWAP_CHAIN_DESC chain{};
    ComPtr<ID3D11Texture2D> backBuffer;
    if (swapChain == nullptr || FAILED(swapChain->GetDesc(&chain))) return {};
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return {};
    D3D11_TEXTURE2D_DESC texture{};
    backBuffer->GetDesc(&texture);
    return {chain.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
        texture.Format == DXGI_FORMAT_R16G16B16A16_FLOAT};
}

struct HostedLatch
{
    bool hdr{};
    bool answered{};
    HostHdrOutputState state{.size = sizeof(HostHdrOutputState)};
};

// Asks the host once per device generation; the per-Present refresh reuses the query.
[[nodiscard]] HostedLatch DecideHostedLatch(IDXGISwapChain* swapChain, const SdrModeInputs& inputs) noexcept
{
    HostedLatch latch;
    g_hostedState.query = nullptr;
    if (!g_configuration.hostedHdrRequested) return latch;
    g_hostedState.query = reinterpret_cast<HostedPresentationState::Query>(
        FindLoadedModuleExport("CS_GetHdrOutputState"));
    latch.answered = g_hostedState.query != nullptr && g_hostedState.query(&latch.state);
    const auto formats = ReadHostedChainFormats(swapChain);
    latch.hdr = DecideHostedHdr({.hostedHdrRequested = true, .enbDetected = inputs.enbDetected,
        .configurationDefaults = inputs.configurationDefaults,
        .hostReportsHdr = HostReportsHdr(latch.answered ? &latch.state : nullptr),
        .chainRgba8Unorm = formats.chainRgba8Unorm, .backBufferFp16 = formats.backBufferFp16});
    return latch;
}

void PrepareHostedHdr(const HostedLatch& latch, ID3D11Device* device) noexcept
{
    // The latch and the paper white precede the build so its constants carry both.
    g_hostedState.paperWhiteNits = latch.hdr ? latch.state.paperWhiteNits : 0.0F;
    g_hostedState.matches.store(latch.hdr, std::memory_order_release);
    g_hostedHdrActive.store(latch.hdr, std::memory_order_release);
    if (!latch.hdr) return;
    // A failed build keeps the latch for the settings menu; nothing substitutes.
    if (!BuildHostedReplacementShaders(device))
    {
        DisableSdrSubstitution();
        RecordFailure(GraphicsObservationFailure::shaderCreationFailed);
    }
}

}  // namespace

void PrepareLatchedModes(SdrModeInputs inputs, IDXGISwapChain* swapChain, ID3D11Device* device) noexcept
{
    const auto hosted = DecideHostedLatch(swapChain, inputs);
    inputs.hostedPresentation = g_configuration.hostedPresentation;
    inputs.hostedHdrRequested = g_configuration.hostedHdrRequested;
    inputs.hostAnswered = hosted.answered;
    inputs.hostedHdr = hosted.hdr;
    // SDR mode goes first: leaving it selects the HDR profile the hosted set is built from.
    (void)PrepareSdrMode(inputs, device);
    PrepareHostedHdr(hosted, device);
}

void ClearLatchedModes() noexcept
{
    SetSdrModeActive(false);
    g_hostedHdrActive.store(false, std::memory_order_release);
}

void RefreshHostedState(ID3D11DeviceContext* context) noexcept
{
    if (!g_hostedHdrActive.load(std::memory_order_acquire) || g_hostedState.query == nullptr) return;
    HostHdrOutputState state{.size = sizeof(HostHdrOutputState)};
    const bool hdr = HostReportsHdr(g_hostedState.query(&state) ? &state : nullptr);
    g_hostedState.matches.store(hdr, std::memory_order_release);
    if (!hdr || state.paperWhiteNits == g_hostedState.paperWhiteNits) return;
    // The scene is absolute, so a new paper white only changes the constants.
    g_hostedState.paperWhiteNits = state.paperWhiteNits;
    if (!PublishReplacementSettings(context, g_activeSettings, g_settingsGeneration))
        DisableSdrSubstitution();
}

bool HdrOperatorActive() noexcept
{
    return g_configuration.productionRequested ||
        g_hostedHdrActive.load(std::memory_order_acquire);
}

float OperatorTransportWhiteNits(double uiPaperWhiteNits) noexcept
{
    return g_hostedHdrActive.load(std::memory_order_acquire) ?
        g_hostedState.paperWhiteNits : static_cast<float>(uiPaperWhiteNits);
}

}  // namespace fo4hdr
