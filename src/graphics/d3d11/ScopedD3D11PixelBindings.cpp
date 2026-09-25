// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ScopedD3D11PixelBindings.hpp"

namespace fo4hdr {

ScopedD3D11PixelBindings::~ScopedD3D11PixelBindings() noexcept
{ Reset(); }

bool ScopedD3D11PixelBindings::Bind(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, const PixelBindingRequest& request) noexcept
{
    if (active_ || context == nullptr) return false;
    if (!request.Valid()) return false;
    if (!CaptureResources(context, request)) return false;
    PrepareContext(context, context1, request);
    ApplyBindings(request);
    active_ = true;
    return true;
}

void ScopedD3D11PixelBindings::Reset() noexcept
{
    if (!active_) return;

    if (bindExtraShaderResource_)
    {
        auto* extraShaderResource = previousExtraShaderResource_.Get();
        context_->PSSetShaderResources(
            extraShaderResourceSlot_, 1, &extraShaderResource);
    }
    if (bindShaderResource_)
    {
        auto* shaderResource = previousShaderResource_.Get();
        auto* sampler = previousSampler_.Get();
        context_->PSSetShaderResources(
            shaderResourceSlot_, 1, &shaderResource);
        context_->PSSetSamplers(samplerSlot_, 1, &sampler);
    }
    auto* constantBuffer = previousConstantBuffer_.Get();
    if (context1_)
        context1_->PSSetConstantBuffers1(constantBufferSlot_, 1,
            &constantBuffer, &previousFirstConstant_,
            &previousNumConstants_);
    else
        context_->PSSetConstantBuffers(
            constantBufferSlot_, 1, &constantBuffer);

    previousSampler_.Reset();
    previousExtraShaderResource_.Reset();
    previousShaderResource_.Reset();
    previousConstantBuffer_.Reset();
    context_ = nullptr;
    context1_ = nullptr;
    bindShaderResource_ = false;
    bindExtraShaderResource_ = false;
    active_ = false;
}

bool ScopedD3D11PixelBindings::ForeignResource(ID3D11ShaderResourceView* previous,
    ID3D11ShaderResourceView* requested, bool rejectCollision) noexcept
{
    return rejectCollision && previous != nullptr && previous != requested;
}

bool ScopedD3D11PixelBindings::CaptureResource(ID3D11DeviceContext* context, ResourceBorrow request,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& previous) noexcept
{
    context->PSGetShaderResources(request.slot, 1, previous.GetAddressOf());
    if (!ForeignResource(previous.Get(), request.resource, request.rejectCollision)) return true;
    previous.Reset();
    return false;
}

bool ScopedD3D11PixelBindings::CaptureResources(ID3D11DeviceContext* context, const PixelBindingRequest& request) noexcept
{
    // An extra SRV can be borrowed independently of the primary SRV/sampler.
    // Apply requested collision checks before changing context state; accepted
    // bindings are borrowed and restored at the end of the draw scope.
    if (request.bindShaderResource && !CaptureResource(context,
            {request.shaderResourceSlot, request.shaderResource, request.rejectShaderResourceCollision},
            previousShaderResource_)) return false;
    if (!request.HasExtraResource()) return true;
    if (CaptureResource(context,
            {request.extraShaderResourceSlot, request.extraShaderResource, request.rejectExtraShaderResourceCollision},
            previousExtraShaderResource_)) return true;
    previousShaderResource_.Reset();
    return false;
}

void ScopedD3D11PixelBindings::PrepareContext(ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
    const PixelBindingRequest& request) noexcept
{
    context_ = context;
    context1_ = context1;
    constantBufferSlot_ = request.constantBufferSlot;
    shaderResourceSlot_ = request.shaderResourceSlot;
    samplerSlot_ = request.samplerSlot;
    bindShaderResource_ = request.bindShaderResource;
    extraShaderResourceSlot_ = request.extraShaderResourceSlot;
    bindExtraShaderResource_ = request.HasExtraResource();
}

void ScopedD3D11PixelBindings::ApplyBindings(const PixelBindingRequest& request) noexcept
{
    if (context1_)
        context1_->PSGetConstantBuffers1(constantBufferSlot_, 1,
            previousConstantBuffer_.GetAddressOf(),
            &previousFirstConstant_, &previousNumConstants_);
    else
        context_->PSGetConstantBuffers(
            constantBufferSlot_, 1, previousConstantBuffer_.GetAddressOf());
    if (bindShaderResource_)
    {
        context_->PSGetSamplers(
            samplerSlot_, 1, previousSampler_.GetAddressOf());
    }

    context_->PSSetConstantBuffers(
        constantBufferSlot_, 1, &request.constantBuffer);
    if (bindShaderResource_)
    {
        context_->PSSetShaderResources(
            shaderResourceSlot_, 1, &request.shaderResource);
        context_->PSSetSamplers(samplerSlot_, 1, &request.sampler);
    }
    if (bindExtraShaderResource_)
        context_->PSSetShaderResources(
            extraShaderResourceSlot_, 1, &request.extraShaderResource);
}

} // namespace fo4hdr
