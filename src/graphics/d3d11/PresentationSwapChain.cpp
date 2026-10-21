// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "PresentationSwapChain.hpp"

#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <new>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

constexpr GUID kNativeBufferLifetime =
    {0xfcaac388, 0xd608, 0x4c4e, {0x9b, 0xf4, 0x07, 0x2a, 0x32, 0x11, 0x60, 0x75}};

class NativeBufferLifetime final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IUnknown>
{
public:
    explicit NativeBufferLifetime(ID3D11Texture2D* buffer) noexcept : buffer_(buffer) {}

private:
    ComPtr<ID3D11Texture2D> buffer_;
};

bool SameDevice(ID3D11Texture2D& buffer, IDXGISwapChain& nativeChain) noexcept
{
    ComPtr<ID3D11Device> device, nativeDevice;
    ComPtr<IUnknown> bufferDevice, chainDevice;
    buffer.GetDevice(&device);
    // Acquire both through D3D11 so an interceptor exposes the same device layer.
    return device && SUCCEEDED(device.As(&bufferDevice)) &&
        SUCCEEDED(nativeChain.GetDevice(IID_PPV_ARGS(&nativeDevice))) &&
        SUCCEEDED(nativeDevice.As(&chainDevice)) &&
        bufferDevice.Get() == chainDevice.Get();
}

bool Fp16BufferDescriptorSupported(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    constexpr auto bindings = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const std::array requirements{
        description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        description.MipLevels == 1, description.ArraySize == 1,
        description.SampleDesc.Count == 1, description.SampleDesc.Quality == 0,
        description.Usage == D3D11_USAGE_DEFAULT, description.CPUAccessFlags == 0,
        description.MiscFlags == 0, (description.BindFlags & bindings) == bindings};
    return std::ranges::all_of(requirements, [](bool supported) { return supported; });
}

bool MatchingBufferDescriptions(const D3D11_TEXTURE2D_DESC& buffer,
    const D3D11_TEXTURE2D_DESC& native) noexcept
{
    return Fp16BufferDescriptorSupported(buffer) &&
        native.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        buffer.Width == native.Width && buffer.Height == native.Height;
}

HRESULT AnchorBuffer(IDXGISwapChain& nativeChain, ID3D11Texture2D& buffer) noexcept
{
    ComPtr<ID3D11Texture2D> nativeBuffer;
    auto result = nativeChain.GetBuffer(0, IID_PPV_ARGS(&nativeBuffer));
    if (FAILED(result)) return result;
    D3D11_TEXTURE2D_DESC actual{}, native{};
    buffer.GetDesc(&actual);
    nativeBuffer->GetDesc(&native);
    if (!MatchingBufferDescriptions(actual, native) || !SameDevice(buffer, nativeChain))
        return DXGI_ERROR_INVALID_CALL;
    auto lifetime = Make<NativeBufferLifetime>(nativeBuffer.Get());
    if (!lifetime) return E_OUTOFMEMORY;
    // Views and recorded context work retain this anchor through the genuine texture.
    return buffer.SetPrivateDataInterface(kNativeBufferLifetime, lifetime.Get());
}

bool ApplicationSwapEffectSupported(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    if (description.SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        return (description.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == 0;
    constexpr auto transportFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    return description.Windowed &&
        description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        description.BufferCount >= 2 && (description.Flags & ~transportFlags) == 0;
}

}  // namespace

bool PresentationFacadeDescriptorSupported(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    return ApplicationSwapEffectSupported(description) &&
        description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0 &&
        description.BufferCount > 0 && description.BufferCount <= DXGI_MAX_SWAP_CHAIN_BUFFERS &&
        (description.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) == 0;
}

namespace {

bool CreationInputsSupported(const PresentationSwapChainCreation& creation) noexcept
{
    return creation.nativeChain != nullptr && creation.buffer != nullptr &&
        creation.acquireBuffer != nullptr &&
        PresentationFacadeDescriptorSupported(creation.applicationDescription);
}

bool NativeDescriptorSupported(const DXGI_SWAP_CHAIN_DESC& description) noexcept
{
    return description.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD && description.BufferCount >= 2 &&
        description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0;
}

UINT BufferUsage(ID3D11Texture2D& buffer) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    buffer.GetDesc(&description);
    UINT usage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    if ((description.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0)
        usage |= DXGI_USAGE_UNORDERED_ACCESS;
    return usage;
}

HRESULT FlushRetiredBuffer(IDXGISwapChain& nativeChain) noexcept
{
    ComPtr<ID3D11Device> device;
    const auto result = nativeChain.GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result)) return result;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return E_FAIL;
    // Standalone textures defer destruction; flush releases their retired native anchors.
    context->Flush();
    return S_OK;
}

class PresentationSwapChain final : public IDXGISwapChain
{
public:
    PresentationSwapChain(const PresentationSwapChainCreation& creation, UINT nativeTearingFlag) noexcept
        : nativeChain_(creation.nativeChain), buffer_(creation.buffer),
          acquireBuffer_(creation.acquireBuffer), retireBuffer_(creation.retireBuffer),
          bufferCount_(creation.applicationDescription.BufferCount),
          bufferUsage_(BufferUsage(*creation.buffer)), nativeTearingFlag_(nativeTearingFlag),
          applicationFormat_(creation.applicationDescription.BufferDesc.Format)
    {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override
    {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIObject) &&
            iid != __uuidof(IDXGIDeviceSubObject) && iid != __uuidof(IDXGISwapChain))
            return E_NOINTERFACE;
        *object = static_cast<IDXGISwapChain*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const auto remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID name, UINT size, const void* data) noexcept override
    {
        return nativeChain_->SetPrivateData(name, size, data);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID name, const IUnknown* object) noexcept override
    {
        return nativeChain_->SetPrivateDataInterface(name, object);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID name, UINT* size, void* data) noexcept override
    {
        return nativeChain_->GetPrivateData(name, size, data);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** parent) noexcept override
    {
        return nativeChain_->GetParent(iid, parent);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** device) noexcept override
    {
        return nativeChain_->GetDevice(iid, device);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) noexcept override
    {
        return nativeChain_->Present(interval, flags);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID iid, void** surface) noexcept override
    {
        if (surface == nullptr) return E_POINTER;
        *surface = nullptr;
        const auto lock = buffersLock_.TryLockShared();
        if (!lock.IsLocked() || index != 0 || !buffer_) return DXGI_ERROR_INVALID_CALL;
        return buffer_->QueryInterface(iid, surface);
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* output) noexcept override
    {
        return nativeChain_->SetFullscreenState(fullscreen, output);
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** output) noexcept override
    {
        return nativeChain_->GetFullscreenState(fullscreen, output);
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* description) noexcept override
    {
        if (description == nullptr) return E_INVALIDARG;
        const auto lock = buffersLock_.TryLockShared();
        if (!lock.IsLocked()) return DXGI_ERROR_INVALID_CALL;
        const auto result = nativeChain_->GetDesc(description);
        if (FAILED(result)) return result;
        description->BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description->BufferCount = bufferCount_;
        description->BufferUsage = bufferUsage_;
        description->SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        description->Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        return result;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height,
        DXGI_FORMAT format, UINT flags) noexcept override
    {
        if (!PublicFormat(format) || count > DXGI_MAX_SWAP_CHAIN_BUFFERS || (flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) != 0 ||
            ((flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0 && nativeTearingFlag_ == 0))
            return DXGI_ERROR_INVALID_CALL;
        const auto lock = buffersLock_.TryLockExclusive();
        if (!lock.IsLocked()) return DXGI_ERROR_INVALID_CALL;
        buffer_.Reset();
        if (retireBuffer_) retireBuffer_(nativeChain_.Get());
        const auto flushed = FlushRetiredBuffer(*nativeChain_.Get());
        if (FAILED(flushed)) return flushed;
        const auto nativeCount = count == 0 ? 0 : std::max<UINT>(2, count);
        // Public descriptor flags omit the private flip chain's immutable tearing bit.
        const auto result = nativeChain_->ResizeBuffers(nativeCount, width, height,
            DXGI_FORMAT_R10G10B10A2_UNORM, flags | nativeTearingFlag_);
        if (FAILED(result)) return result;
        if (count != 0) bufferCount_ = count;
        ComPtr<ID3D11Texture2D> replacement;
        const auto acquired = acquireBuffer_(nativeChain_.Get(), &replacement);
        if (FAILED(acquired)) return RetireFailedBuffer(acquired);
        if (!replacement) return RetireFailedBuffer(E_FAIL);
        const auto anchored = AnchorBuffer(*nativeChain_.Get(), *replacement.Get());
        if (FAILED(anchored)) return RetireFailedBuffer(anchored);
        buffer_ = std::move(replacement);
        bufferUsage_ = BufferUsage(*buffer_.Get());
        return result;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* target) noexcept override
    {
        if (target == nullptr) return DXGI_ERROR_INVALID_CALL;
        auto nativeTarget = *target;
        if (!PublicFormat(target->Format)) return DXGI_ERROR_INVALID_CALL;
        nativeTarget.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        return nativeChain_->ResizeTarget(&nativeTarget);
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) noexcept override
    {
        return nativeChain_->GetContainingOutput(output);
    }

    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) noexcept override
    {
        return nativeChain_->GetFrameStatistics(statistics);
    }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) noexcept override
    {
        return nativeChain_->GetLastPresentCount(count);
    }

private:
    // The application may restate its creation format; the public buffer stays FP16.
    bool PublicFormat(DXGI_FORMAT format) const noexcept
    {
        return format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            format == applicationFormat_;
    }

    HRESULT RetireFailedBuffer(HRESULT failure) noexcept
    {
        if (retireBuffer_) retireBuffer_(nativeChain_.Get());
        return failure;
    }

    std::atomic<ULONG> references_{1};
    ComPtr<IDXGISwapChain> nativeChain_;
    ComPtr<ID3D11Texture2D> buffer_;
    AcquirePresentationBuffer acquireBuffer_;
    RetirePresentationBuffer retireBuffer_;
    Microsoft::WRL::Wrappers::SRWLock buffersLock_;
    UINT bufferCount_;
    UINT bufferUsage_;
    const UINT nativeTearingFlag_;
    const DXGI_FORMAT applicationFormat_;
};

}  // namespace

HRESULT CreatePresentationSwapChain(
    const PresentationSwapChainCreation& creation, IDXGISwapChain** result) noexcept
{
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!CreationInputsSupported(creation))
        return DXGI_ERROR_INVALID_CALL;
    DXGI_SWAP_CHAIN_DESC native{};
    const auto described = creation.nativeChain->GetDesc(&native);
    if (FAILED(described)) return described;
    if (!NativeDescriptorSupported(native))
        return DXGI_ERROR_INVALID_CALL;
    const auto requestedTearing = creation.applicationDescription.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if ((requestedTearing & ~native.Flags) != 0)
        return DXGI_ERROR_INVALID_CALL;
    auto* chain = new (std::nothrow) PresentationSwapChain(creation,
        native.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    if (chain == nullptr) return E_OUTOFMEMORY;
    const auto anchored = AnchorBuffer(*creation.nativeChain, *creation.buffer);
    if (FAILED(anchored))
    {
        chain->Release();
        return anchored;
    }
    *result = chain;
    return S_OK;
}

} // namespace fo4hdr
