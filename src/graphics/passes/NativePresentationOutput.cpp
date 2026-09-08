// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationPass.hpp"

#include <array>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

bool SameObject(IUnknown* left, IUnknown* right) noexcept
{
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> a, b;
    if (FAILED(left->QueryInterface(IID_PPV_ARGS(&a)))) return false;
    if (FAILED(right->QueryInterface(IID_PPV_ARGS(&b)))) return false;
    return a && a.Get() == b.Get();
}

bool FlipChainLayout(const DXGI_SWAP_CHAIN_DESC1& value) noexcept
{
    if (value.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) return false;
    if (value.BufferCount != 2 || value.Stereo) return false;
    return value.SampleDesc.Count == 1 && value.SampleDesc.Quality == 0;
}

bool ChainDescription(const DXGI_SWAP_CHAIN_DESC1& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (!FlipChainLayout(value)) return false;
    if (value.Format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
    if (value.Width != configuration.width || value.Height != configuration.height) return false;
    if ((value.BufferUsage & DXGI_USAGE_SHADER_INPUT) != 0) return false;
    return (value.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0;
}

bool DisplayFormatSupported(ID3D11Device* device) noexcept
{
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support))) return false;
    return (support & D3D11_FORMAT_SUPPORT_DISPLAY) != 0;
}

HRESULT ChainDeviceQualification(IDXGISwapChain3* chain, ID3D11Device* device) noexcept
{
    ComPtr<ID3D11Device> actual;
    const auto result = chain->GetDevice(IID_PPV_ARGS(&actual));
    if (FAILED(result)) return result;
    if (!SameObject(actual.Get(), device)) return E_INVALIDARG;
    return DisplayFormatSupported(device) ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

HRESULT CheckedChain(IDXGISwapChain* chain, ID3D11Device* device,
    const PresentationConfiguration& configuration, ComPtr<IDXGISwapChain3>& candidate) noexcept
{
    if (chain == nullptr) return E_INVALIDARG;
    auto result = chain->QueryInterface(IID_PPV_ARGS(&candidate));
    if (FAILED(result)) return result;
    if (!candidate) return E_NOINTERFACE;
    DXGI_SWAP_CHAIN_DESC1 description{};
    result = candidate->GetDesc1(&description);
    if (FAILED(result)) return result;
    if (!ChainDescription(description, configuration)) return E_INVALIDARG;
    return ChainDeviceQualification(candidate.Get(), device);
}

HRESULT ApplyColorSpace(IDXGISwapChain3* chain, bool hdr) noexcept
{
    UINT support{};
    const auto colorSpace = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const auto result = chain->CheckColorSpaceSupport(colorSpace, &support);
    if (FAILED(result)) return result;
    if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) return DXGI_ERROR_UNSUPPORTED;
    return chain->SetColorSpace1(colorSpace);
}

HRESULT SupportedColorSpace(IDXGISwapChain3* chain, DXGI_COLOR_SPACE_TYPE space) noexcept
{
    UINT support{};
    const auto result = chain->CheckColorSpaceSupport(space, &support);
    if (FAILED(result)) return result;
    return (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0
        ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

HRESULT SupportedNativeSpaces(IDXGISwapChain3* chain) noexcept
{
    constexpr std::array spaces{DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    for (const auto space : spaces)
    {
        const auto result = SupportedColorSpace(chain, space);
        if (FAILED(result)) return result;
    }
    return S_OK;
}

HRESULT ApplyNativeBaseline(IDXGISwapChain3* chain, bool previouslyQualified) noexcept
{
    if (!previouslyQualified)
    {
        const auto result = SupportedNativeSpaces(chain);
        if (FAILED(result)) return result;
    }
    const auto result = chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    if (FAILED(result)) return result;
    return chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
}

bool DeviceLost(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
}

}  // namespace

HRESULT PresentationPass::PrepareOutputChain(IDXGISwapChain* chain,
    ComPtr<IDXGISwapChain3>& candidate, ChainBuffer& prepared) const noexcept
{
    const auto result = CheckedChain(chain, device_.Get(), configuration_, candidate);
    if (FAILED(result)) return result;
    return PrepareBuffer(candidate.Get(), prepared);
}

void PresentationPass::CommitOutputChain(ComPtr<IDXGISwapChain3> candidate,
    ChainBuffer prepared, bool hdr) noexcept
{
    buffer_ = std::move(prepared);
    chain_ = std::move(candidate);
    hdrColorSpace_ = hdr;
}

HRESULT PresentationPass::PrepareChain(IDXGISwapChain* chain) noexcept
{
    if (!CanPrepareChain()) return E_UNEXPECTED;
    ComPtr<IDXGISwapChain3> candidate;
    ChainBuffer prepared;
    auto result = PrepareOutputChain(chain, candidate, prepared);
    if (FAILED(result)) return result;
    result = ApplyColorSpace(candidate.Get(), true);
    if (FAILED(result)) return result;
    CommitOutputChain(std::move(candidate), std::move(prepared), true);
    return S_OK;
}

HRESULT PresentationPass::PrepareNativeChain(IDXGISwapChain* chain, bool previouslyQualified) noexcept
{
    if (!CanPrepareChain()) return E_UNEXPECTED;
    ComPtr<IDXGISwapChain3> candidate;
    ChainBuffer prepared;
    auto result = PrepareOutputChain(chain, candidate, prepared);
    if (FAILED(result)) return result;
    result = ApplyNativeBaseline(candidate.Get(), previouslyQualified);
    if (FAILED(result)) return result;
    CommitOutputChain(std::move(candidate), std::move(prepared), false);
    return S_OK;
}

bool PresentationPass::CanPrepareChain() const noexcept
{
    if (!Active() || executing_) return false;
    if (chain_) return false;
    return configuration_.scenes->MatchesContext(device_.Get(), context_.Get());
}

HRESULT PresentationPass::CreateTerminalView(ChainBuffer& prepared) const noexcept
{
    const auto result = device_->CreateUnorderedAccessView(prepared.texture.Get(), nullptr, &prepared.target);
    if (FAILED(result)) return result;
    if (!prepared.target) return E_FAIL;
    return QualifiedTarget(prepared.target.Get(), nullptr) ? S_OK : E_INVALIDARG;
}

HRESULT PresentationPass::PrepareBuffer(IDXGISwapChain3* chain,
    ChainBuffer& prepared) const noexcept
{
    // DXGI keeps the D3D11 buffer-zero view current across Present. Other indices
    // need not support writable views, even when their texture bind flags match.
    auto result = chain->GetBuffer(0, IID_PPV_ARGS(&prepared.texture));
    if (FAILED(result)) return result;
    if (!prepared.texture) return E_FAIL;
    return CreateTerminalView(prepared);
}

bool PresentationPass::SameChain(IDXGISwapChain* chain) const noexcept
{
    return chain_ && SameObject(chain, chain_.Get());
}

ID3D11UnorderedAccessView* PresentationPass::CurrentTarget() const noexcept
{
    return chain_ ? buffer_.target.Get() : nullptr;
}

bool PresentationPass::BeginNativeFrame(PresentationScope scope) noexcept
{
    if (!CanExecute(scope) || CurrentTarget() == nullptr) return false;
    if (!SetHdrColorSpace(false)) return false;
    CloseFrame();
    if (!configuration_.scenes->BeginFrame()) return false;
    framePrepared_ = true;
    return true;
}

bool PresentationPass::PrepareNativeRedirect(PresentationScope scope) noexcept
{
    if (!CanExecute(scope) || !framePrepared_) return false;
    auto* target = CurrentTarget();
    if (target == nullptr || !SetHdrColorSpace(false)) return false;
    return ClearTerminal(target);
}

bool PresentationPass::RestoreNativeSdrSpace() noexcept
{
    return Active() && OnOwnerThread() && !executing_ && SetHdrColorSpace(false);
}

bool PresentationPass::FinishNativeFrame(bool redirected, bool hdrEligible,
    PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    auto* target = CurrentTarget();
    if (target == nullptr) return false;
    bool completed = hdrEligible && ComposeHdrChain(target, scope);
    if (!completed)
        completed = redirected ? ComposeNativeFallback(target) : SetHdrColorSpace(false);
    CloseFrame();
    return completed;
}

bool PresentationPass::ComposeHdrChain(ID3D11UnorderedAccessView* target,
    PresentationScope scope) noexcept
{
    if (!framePrepared_) return false;
    if (!SetHdrColorSpace(true)) return false;
    return ExecuteCurrentScene(target, scope);
}

bool PresentationPass::ComposeSdrChain(ID3D11UnorderedAccessView* target) noexcept
{
    if (!framePrepared_) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (frame == 0 || sdrFrame_ != frame) return false;
    if (!SetHdrColorSpace(false)) return false;
    return CopySdrCodes(sdrProxy_.source.Get(), target);
}

void PresentationPass::ComposeChain(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return;
    auto* target = CurrentTarget();
    if (target == nullptr) return;
    if (ComposeHdrChain(target, scope)) return;
    static_cast<void>(ComposeNativeFallback(target));
}

bool PresentationPass::SetHdrColorSpace(bool hdr) noexcept
{
    if (!chain_) return false;
    if (hdr == hdrColorSpace_) return true;
    if (FAILED(ApplyColorSpace(chain_.Get(), hdr))) return false;
    hdrColorSpace_ = hdr;
    return true;
}

HRESULT PresentationPass::Present(IDXGISwapChain* chain, UINT interval, UINT flags,
    PresentationScope scope) noexcept
{
    if (chain == nullptr) return E_INVALIDARG;
    PrepareNativePresent(chain, flags, scope);
    const auto result = chain->Present(interval, flags);
    CompleteNativePresent(chain, result);
    return result;
}

bool PresentationPass::OwnedChainCall(IDXGISwapChain* chain) const noexcept
{
    return Active() && !executing_ && SameChain(chain);
}

void PresentationPass::PrepareNativePresent(IDXGISwapChain* chain, UINT flags,
    PresentationScope scope) noexcept
{
    if (chain == nullptr) return;
    const bool owned = OwnedChainCall(chain);
    const bool test = (flags & DXGI_PRESENT_TEST) != 0;
    if (owned && !test)
    {
        ComposeChain(scope);
        CloseFrame();
    }
}

void PresentationPass::CompleteNativePresent(IDXGISwapChain* chain, HRESULT nativeResult) noexcept
{
    if (chain == nullptr) return;
    const bool owned = OwnedChainCall(chain);
    if (owned && DeviceLost(nativeResult))
    {
        lifetime_.Retire();
        CloseFrame();
    }
}

}  // namespace fo4hdr
