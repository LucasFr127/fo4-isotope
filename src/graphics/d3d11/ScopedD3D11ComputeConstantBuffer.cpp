// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ScopedD3D11ComputeConstantBuffer.hpp"

namespace fo4hdr {

ScopedD3D11ComputeConstantBuffer::~ScopedD3D11ComputeConstantBuffer() noexcept
{ Restore(); }

void ScopedD3D11ComputeConstantBuffer::Capture(ID3D11DeviceContext* context, ID3D11DeviceContext1* cachedContext1,
    UINT slot) noexcept
{
    Restore();
    if (!context) return;

    context_ = context;
    context1_ = cachedContext1;
    slot_ = slot;
    if (context1_)
        context1_->CSGetConstantBuffers1(slot_, 1, &buffer_, &firstConstant_,
            &constantCount_);
    else
        context_->CSGetConstantBuffers(slot_, 1, &buffer_);
}

void ScopedD3D11ComputeConstantBuffer::Restore() noexcept
{
    if (!context_) return;

    if (context1_)
        context1_->CSSetConstantBuffers1(slot_, 1, &buffer_, &firstConstant_,
            &constantCount_);
    else
        context_->CSSetConstantBuffers(slot_, 1, &buffer_);

    if (buffer_) {
        buffer_->Release();
        buffer_ = nullptr;
    }
    context_ = nullptr;
    context1_ = nullptr;
}

} // namespace fo4hdr
