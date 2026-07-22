// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScopedD3D11PixelBindings.hpp"
#include "../OperatorConstants.hpp"

namespace fo4hdr {

ScopedD3D11PixelBindings::ScopedD3D11PixelBindings(ID3D11DeviceContext1* context,
    ID3D11Buffer* constants) noexcept : context_(context), constants_(constants)
{
    if (constants_ == nullptr) return;
    context_->PSGetConstantBuffers(kProductionOperatorConstantSlot, 1, &previousConstants_);
    context_->PSGetConstantBuffers1(kProductionOperatorConstantSlot, 1, nullptr,
        &firstConstant_, &constantCount_);
    context_->PSGetShaderResources(kProductionOperatorLutSlot, 1, &previousSource_);
    context_->PSGetSamplers(kProductionOperatorSamplerSlot, 1, &previousSampler_);
}

ScopedD3D11PixelBindings::~ScopedD3D11PixelBindings() noexcept
{
    if (!bound_) return;
    auto* constants = previousConstants_.Get();
    auto* source = previousSource_.Get();
    auto* sampler = previousSampler_.Get();
    context_->PSSetShaderResources(kProductionOperatorLutSlot, 1, &source);
    context_->PSSetSamplers(kProductionOperatorSamplerSlot, 1, &sampler);
    context_->PSSetConstantBuffers1(kProductionOperatorConstantSlot, 1,
        &constants, &firstConstant_, &constantCount_);
}

bool ScopedD3D11PixelBindings::Ready() const noexcept
{
    if (constants_ == nullptr) return true;
    // A bound empty window is legal, but does not qualify a legacy default here.
    if (previousConstants_ && constantCount_ == 0) return false;
    if (firstConstant_ % 16 != 0 || constantCount_ % 16 != 0) return false;
    return constantCount_ <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
}

void ScopedD3D11PixelBindings::Bind(ID3D11ShaderResourceView* source,
    ID3D11SamplerState* sampler) noexcept
{
    if (constants_ == nullptr) return;
    context_->PSSetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants_);
    context_->PSSetShaderResources(kProductionOperatorLutSlot, 1, &source);
    context_->PSSetSamplers(kProductionOperatorSamplerSlot, 1, &sampler);
    bound_ = true;
}

}  // namespace fo4hdr
