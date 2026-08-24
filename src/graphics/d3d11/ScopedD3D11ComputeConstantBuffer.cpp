// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScopedD3D11ComputeConstantBuffer.hpp"

namespace fo4hdr {

ScopedD3D11ComputeConstantBuffer::ScopedD3D11ComputeConstantBuffer(
    ID3D11DeviceContext1* context) noexcept : context_(context)
{}

ScopedD3D11ComputeConstantBuffer::~ScopedD3D11ComputeConstantBuffer() noexcept
{
    if (bound_) Restore();
}

void ScopedD3D11ComputeConstantBuffer::Capture() noexcept
{
    context_->CSGetConstantBuffers1(0, 1, &constants_, &firstConstant_, &constantCount_);
}

bool ScopedD3D11ComputeConstantBuffer::Ready() const noexcept
{
    if (firstConstant_ % 16 != 0 || constantCount_ % 16 != 0) return false;
    return constantCount_ <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
}

void ScopedD3D11ComputeConstantBuffer::Bind(ID3D11Buffer* constants) noexcept
{
    bound_ = true;
    context_->CSSetConstantBuffers(0, 1, &constants);
}

void ScopedD3D11ComputeConstantBuffer::Restore() noexcept
{
    if (context_ == nullptr) return;
    auto* constants = constants_.Get();
    context_->CSSetConstantBuffers1(0, 1, &constants, &firstConstant_, &constantCount_);
    bound_ = false;
    context_ = nullptr;
}

}  // namespace fo4hdr
