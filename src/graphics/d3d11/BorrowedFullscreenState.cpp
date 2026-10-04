// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "BorrowedFullscreenState.hpp"

namespace fo4hdr {

BorrowedFullscreenState::BorrowedFullscreenState(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, ID3D11Resource* destination) noexcept :
    context_(context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? context : nullptr),
    context1_(context1),
    outputs_(context_, context1, destination)
{
    if (!context_) return;
    CaptureGraphics();
    // Disabling GS+SO leaves foreign buffer offsets and DrawAuto counts untouched.
    context_->GSSetShader(nullptr, nullptr, 0);
}

BorrowedFullscreenState::~BorrowedFullscreenState() noexcept
{
    if (context_) RestoreGraphics();
}

void BorrowedFullscreenState::CaptureGraphics() noexcept
{
    vertex_.Capture(context_, &ID3D11DeviceContext::VSGetShader);
    hull_.Capture(context_, &ID3D11DeviceContext::HSGetShader);
    domain_.Capture(context_, &ID3D11DeviceContext::DSGetShader);
    geometry_.Capture(context_, &ID3D11DeviceContext::GSGetShader);
    pixel_.Capture(context_, &ID3D11DeviceContext::PSGetShader);
    context_->IAGetInputLayout(&layout_);
    context_->IAGetPrimitiveTopology(&topology_);
    context_->RSGetState(&rasterizer_);
    context_->RSGetViewports(&viewportCount_, viewports_.data());
    context_->RSGetScissorRects(&scissorCount_, scissors_.data());
    context_->OMGetBlendState(&blend_, blendFactor_.data(), &sampleMask_);
    context_->OMGetDepthStencilState(&depth_, &stencilReference_);
    context_->PSGetShaderResources(0, static_cast<UINT>(resources_.size()), resources_.data());
    if (context1_)
    {
        context1_->PSGetConstantBuffers1(0, 1, &constants_, &firstConstant_, &constantCount_);
        context1_->VSGetConstantBuffers1(0, 1, &vertexConstants_,
            &firstVertexConstant_, &vertexConstantCount_);
    }
    else
    {
        context_->PSGetConstantBuffers(0, 1, &constants_);
        context_->VSGetConstantBuffers(0, 1, &vertexConstants_);
    }
}

void BorrowedFullscreenState::RestoreGraphics() noexcept
{
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->PSSetShaderResources(0, static_cast<UINT>(resources_.size()), resources_.data());
    for (auto* resource : resources_) if (resource) resource->Release();
    if (context1_)
    {
        auto* buffer = constants_.Get();
        context1_->PSSetConstantBuffers1(0, 1, &buffer, &firstConstant_, &constantCount_);
        auto* vertexBuffer = vertexConstants_.Get();
        context1_->VSSetConstantBuffers1(0, 1, &vertexBuffer,
            &firstVertexConstant_, &vertexConstantCount_);
    }
    else
    {
        auto* buffer = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &buffer);
        auto* vertexBuffer = vertexConstants_.Get();
        context_->VSSetConstantBuffers(0, 1, &vertexBuffer);
    }
    vertex_.Restore(context_, &ID3D11DeviceContext::VSSetShader);
    hull_.Restore(context_, &ID3D11DeviceContext::HSSetShader);
    domain_.Restore(context_, &ID3D11DeviceContext::DSSetShader);
    geometry_.Restore(context_, &ID3D11DeviceContext::GSSetShader);
    pixel_.Restore(context_, &ID3D11DeviceContext::PSSetShader);
    context_->IASetInputLayout(layout_.Get());
    context_->IASetPrimitiveTopology(topology_);
    context_->RSSetState(rasterizer_.Get());
    context_->RSSetViewports(viewportCount_, viewports_.data());
    context_->RSSetScissorRects(scissorCount_, scissors_.data());
    context_->OMSetBlendState(blend_.Get(), blendFactor_.data(), sampleMask_);
    context_->OMSetDepthStencilState(depth_.Get(), stencilReference_);
}

} // namespace fo4hdr
