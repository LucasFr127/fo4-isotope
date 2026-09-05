// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativePresentationProxy.hpp"

#include <array>
#include <exception>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT inputCount = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT outputCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr std::array getters{&ID3D11DeviceContext::VSGetShaderResources,
    &ID3D11DeviceContext::HSGetShaderResources, &ID3D11DeviceContext::DSGetShaderResources,
    &ID3D11DeviceContext::GSGetShaderResources, &ID3D11DeviceContext::PSGetShaderResources,
    &ID3D11DeviceContext::CSGetShaderResources};
constexpr std::array setters{&ID3D11DeviceContext::VSSetShaderResources,
    &ID3D11DeviceContext::HSSetShaderResources, &ID3D11DeviceContext::DSSetShaderResources,
    &ID3D11DeviceContext::GSSetShaderResources, &ID3D11DeviceContext::PSSetShaderResources,
    &ID3D11DeviceContext::CSSetShaderResources};

ComPtr<IUnknown> Identity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object == nullptr) return identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) identity.Reset();
    return identity;
}

bool SameIdentity(IUnknown* left, IUnknown* right) noexcept
{
    const auto first = Identity(left);
    const auto second = Identity(right);
    return first && second && first.Get() == second.Get();
}

bool QualifiedContext(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (device == nullptr || context == nullptr) return false;
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> actualDevice;
    ComPtr<ID3D11DeviceContext> actualContext;
    context->GetDevice(&actualDevice);
    device->GetImmediateContext(&actualContext);
    return SameIdentity(device, actualDevice.Get()) && SameIdentity(context, actualContext.Get());
}

bool QualifiedExtent(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.Width == 0 || description.Height == 0) return false;
    return description.Width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        description.Height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

bool QualifiedLayout(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.MipLevels != 1 || description.ArraySize != 1) return false;
    return description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0;
}

bool QualifiedStorage(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0) return false;
    if (description.MiscFlags != 0) return false;
    if ((description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) return false;
    return description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        description.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

bool QualifiedBuffer(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    return QualifiedExtent(description) && QualifiedLayout(description) && QualifiedStorage(description);
}

bool SupportsProxy(ID3D11Device* device) noexcept
{
    UINT support{};
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
        D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &support))) return false;
    return (support & required) == required;
}

bool CreateProxy(ID3D11Device* device, D3D11_TEXTURE2D_DESC description,
    ComPtr<ID3D11Texture2D>& texture) noexcept
{
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device->CreateTexture2D(&description, nullptr, &texture)) && texture;
}

class OwnerWork
{
public:
    explicit OwnerWork(bool& executing) noexcept : executing_(executing) { executing_ = true; }
    ~OwnerWork() noexcept { executing_ = false; }
    OwnerWork(const OwnerWork&) = delete;
    OwnerWork& operator=(const OwnerWork&) = delete;
private:
    bool& executing_;
};

template <class View>
bool ExcludeProxy(View*& view, IUnknown* proxyIdentity, bool& changed) noexcept
{
    if (view == nullptr) return true;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    const auto identity = Identity(resource.Get());
    if (!identity) return false;
    if (identity.Get() != proxyIdentity) return true;
    view->Release();
    view = nullptr;
    changed = true;
    return true;
}

template <class View, std::size_t Count>
bool ExcludeViews(std::array<View*, Count>& views, IUnknown* proxyIdentity,
    bool& changed) noexcept
{
    for (auto& view : views)
        if (!ExcludeProxy(view, proxyIdentity, changed)) return false;
    return true;
}

template <class View, std::size_t Count>
void ReleaseViews(std::array<View*, Count>& views) noexcept
{
    for (auto* view : views)
        if (view != nullptr) view->Release();
}

class ProxyBindings
{
public:
    explicit ProxyBindings(ID3D11DeviceContext* context) noexcept
    {
        for (std::size_t stage = 0; stage < getters.size(); ++stage)
            (context->*getters[stage])(0, inputCount, inputs_[stage].data());
        context->OMGetRenderTargets(outputCount, outputs_.data(), &depth_);
        for (UINT index = 0; index < outputCount; ++index)
            if (outputs_[index] != nullptr) outputSpan_ = index + 1;
    }
    ~ProxyBindings() noexcept
    {
        for (auto& stage : inputs_) ReleaseViews(stage);
        ReleaseViews(outputs_);
    }
    ProxyBindings(const ProxyBindings&) = delete;
    ProxyBindings& operator=(const ProxyBindings&) = delete;

    bool Exclude(IUnknown* proxyIdentity) noexcept
    {
        for (std::size_t stage = 0; stage < inputs_.size(); ++stage)
            if (!ExcludeViews(inputs_[stage], proxyIdentity, inputChanged_[stage])) return false;
        return ExcludeViews(outputs_, proxyIdentity, outputChanged_);
    }

    void Apply(ID3D11DeviceContext* context) const noexcept
    {
        for (std::size_t stage = 0; stage < inputs_.size(); ++stage)
            if (inputChanged_[stage])
                (context->*setters[stage])(0, inputCount, inputs_[stage].data());
        if (!outputChanged_) return;
        // KEEP preserves every OM UAV and its counter; no CS UAV is touched.
        context->OMSetRenderTargetsAndUnorderedAccessViews(outputSpan_, outputs_.data(),
            depth_.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    }

private:
    std::array<std::array<ID3D11ShaderResourceView*, inputCount>, getters.size()> inputs_{};
    std::array<ID3D11RenderTargetView*, outputCount> outputs_{};
    ComPtr<ID3D11DepthStencilView> depth_;
    std::array<bool, getters.size()> inputChanged_{};
    UINT outputSpan_{};
    bool outputChanged_{};
};

}  // namespace

NativePresentationProxy::~NativePresentationProxy() noexcept
{
    if (!Retire()) std::terminate();
}

bool NativePresentationProxy::CaptureBackBuffer(ID3D11Device* device,
    IDXGISwapChain* chain, Resources& candidate) noexcept
{
    if (chain == nullptr) return false;
    ComPtr<ID3D11Device> chainDevice;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&chainDevice)))) return false;
    if (!SameIdentity(device, chainDevice.Get())) return false;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&candidate.backBuffer)))) return false;
    if (!candidate.backBuffer) return false;
    D3D11_TEXTURE2D_DESC description{};
    candidate.backBuffer->GetDesc(&description);
    if (!QualifiedBuffer(description)) return false;
    candidate.nativeFormat = description.Format;
    candidate.backBufferIdentity = Identity(candidate.backBuffer.Get());
    return candidate.backBufferIdentity != nullptr;
}

bool NativePresentationProxy::PrepareProxy(ID3D11Device* device, Resources& candidate) noexcept
{
    if (!SupportsProxy(device)) return false;
    D3D11_TEXTURE2D_DESC description{};
    candidate.backBuffer->GetDesc(&description);
    if (!CreateProxy(device, description, candidate.proxy)) return false;
    if (FAILED(device->CreateRenderTargetView(candidate.proxy.Get(), nullptr, &candidate.target)))
        return false;
    if (FAILED(device->CreateShaderResourceView(candidate.proxy.Get(), nullptr, &candidate.source)))
        return false;
    if (!candidate.target || !candidate.source) return false;
    candidate.proxyIdentity = Identity(candidate.proxy.Get());
    return candidate.proxyIdentity != nullptr;
}

bool NativePresentationProxy::PrepareResources(ID3D11Device* device,
    ID3D11DeviceContext* context, IDXGISwapChain* chain, Resources& candidate) noexcept
{
    if (!QualifiedContext(device, context)) return false;
    if (!CaptureBackBuffer(device, chain, candidate)) return false;
    if (!PrepareProxy(device, candidate)) return false;
    candidate.context = context;
    return true;
}

bool NativePresentationProxy::Prepare(ID3D11Device* device, ID3D11DeviceContext* context,
    IDXGISwapChain* chain) noexcept
{
    if (resources_.context || executing_) return false;
    const OwnerWork work{executing_};
    Resources candidate;
    if (!PrepareResources(device, context, chain, candidate)) return false;
    resources_ = std::move(candidate);
    ownerThread_ = std::this_thread::get_id();
    return true;
}

bool NativePresentationProxy::OnOwnerThread() const noexcept
{
    return ownerThread_ == std::this_thread::get_id();
}

bool NativePresentationProxy::CanResolve() const noexcept
{
    if (!OnOwnerThread()) return false;
    if (executing_) return false;
    return resources_.context != nullptr;
}

ID3D11RenderTargetView* NativePresentationProxy::ResolveRenderTarget(
    ID3D11RenderTargetView* nativeTarget) const noexcept
{
    if (!CanResolve() || nativeTarget == nullptr) return nullptr;
    const OwnerWork work{executing_};
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    nativeTarget->GetDesc(&description);
    if (description.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return nullptr;
    if (description.Texture2D.MipSlice != 0 || description.Format != resources_.nativeFormat) return nullptr;
    ComPtr<ID3D11Resource> resource;
    nativeTarget->GetResource(&resource);
    const auto identity = Identity(resource.Get());
    if (!identity || identity.Get() != resources_.backBufferIdentity.Get()) return nullptr;
    return resources_.target.Get();
}

bool NativePresentationProxy::Retire() noexcept
{
    if (executing_) return false;
    if (!resources_.context) return true;
    if (!OnOwnerThread()) return false;
    const OwnerWork work{executing_};
    {
        ProxyBindings bindings{resources_.context.Get()};
        if (!bindings.Exclude(resources_.proxyIdentity.Get())) return false;
        bindings.Apply(resources_.context.Get());
    }
    resources_ = {};
    ownerThread_ = {};
    return true;
}

}  // namespace fo4hdr
