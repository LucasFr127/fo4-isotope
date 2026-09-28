// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "NativeReferenceGradePass.hpp"

#include "../d3d11/ScopedD3D11ComputeConstantBuffer.hpp"

#include <algorithm>
#include <cstddef>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] bool IsNativeReferenceView(const D3D11_SHADER_RESOURCE_VIEW_DESC& description) noexcept
{
    return description.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D &&
        description.Texture3D.MostDetailedMip == 0 &&
        description.Texture3D.MipLevels == 1;
}

[[nodiscard]] bool IsNativeReferenceTexture(const D3D11_TEXTURE3D_DESC& description) noexcept
{
    const bool cube = description.Width == 16 && description.Height == 16 && description.Depth == 16;
    return cube && description.MipLevels >= 1 && description.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

[[nodiscard]] bool CaptureNativeReferenceConstants(ID3D11DeviceContext1* context1,
    NativeReferenceInputs& inputs) noexcept
{
    ID3D11Buffer* constants{};
    context1->PSGetConstantBuffers1(2, 1, &constants,
        &inputs.firstConstant, &inputs.constantCount);
    inputs.constants.Attach(constants);
    if (!inputs.constants || inputs.constantCount < 2) return false;
    D3D11_BUFFER_DESC description{};
    inputs.constants->GetDesc(&description);
    return inputs.firstConstant <= description.ByteWidth / 16 &&
        description.ByteWidth - inputs.firstConstant * 16 >= 32;
}

class ScopedNativeReferenceComputeState
{
public:
    ScopedNativeReferenceComputeState(ID3D11DeviceContext* context,
        ID3D11DeviceContext1* context1) noexcept : context_(context)
    {
        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
        instanceCount_ = static_cast<UINT>(instances.size());
        ID3D11ComputeShader* shader{};
        context_->CSGetShader(&shader, instances.data(), &instanceCount_);
        shader_.Attach(shader);
        for (UINT i = 0; i < instanceCount_; ++i) instances_[i].Attach(instances[i]);
        std::array<ID3D11ShaderResourceView*, 4> resources{};
        context_->CSGetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());
        for (std::size_t i = 0; i < resources.size(); ++i) resources_[i].Attach(resources[i]);
        ID3D11UnorderedAccessView* unordered{};
        context_->CSGetUnorderedAccessViews(0, 1, &unordered);
        unordered_.Attach(unordered);
        constant_.Capture(context_, context1, 2);
    }

    ScopedNativeReferenceComputeState(const ScopedNativeReferenceComputeState&) = delete;
    ScopedNativeReferenceComputeState& operator=(const ScopedNativeReferenceComputeState&) = delete;
    ~ScopedNativeReferenceComputeState() noexcept { Restore(); }

    void Restore() noexcept
    {
        if (!context_) return;
        std::array<ID3D11ShaderResourceView*, 4> clearResources{};
        ID3D11UnorderedAccessView* clearUnordered{};
        context_->CSSetShaderResources(0, static_cast<UINT>(clearResources.size()),
            clearResources.data());
        context_->CSSetUnorderedAccessViews(0, 1, &clearUnordered, nullptr);
        std::array<ID3D11ShaderResourceView*, 4> resources{};
        for (std::size_t i = 0; i < resources.size(); ++i) resources[i] = resources_[i].Get();
        auto* unordered = unordered_.Get();
        context_->CSSetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());
        context_->CSSetUnorderedAccessViews(0, 1, &unordered, nullptr);
        constant_.Restore();
        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
        for (UINT i = 0; i < instanceCount_; ++i) instances[i] = instances_[i].Get();
        context_->CSSetShader(shader_.Get(), instanceCount_ ? instances.data() : nullptr,
            instanceCount_);
        context_ = nullptr;
    }

private:
    ID3D11DeviceContext* context_{};
    ComPtr<ID3D11ComputeShader> shader_;
    std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> instances_{};
    UINT instanceCount_{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> resources_{};
    ComPtr<ID3D11UnorderedAccessView> unordered_;
    fo4hdr::ScopedD3D11ComputeConstantBuffer constant_;
};

} // namespace

[[nodiscard]] bool IsQualifiedNativeReferenceLook(ID3D11ShaderResourceView* view) noexcept
{
    if (!view) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    if (!IsNativeReferenceView(viewDescription)) return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture3D> texture;
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE3D_DESC description{};
    texture->GetDesc(&description);
    return IsNativeReferenceTexture(description);
}

[[nodiscard]] bool CaptureNativeReferenceInputs(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, NativeReferenceInputs& inputs) noexcept
{
    if (!context || !context1) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue{};
    context->GetPredication(&predicate, &predicateValue);
    if (predicate) return false;
    std::array<ID3D11ShaderResourceView*, 4> looks{};
    context->PSGetShaderResources(3, static_cast<UINT>(looks.size()), looks.data());
    for (std::size_t i = 0; i < looks.size(); ++i) inputs.looks[i].Attach(looks[i]);
    if (!std::ranges::all_of(inputs.looks,
            [](const auto& view) { return IsQualifiedNativeReferenceLook(view.Get()); })) return false;
    return CaptureNativeReferenceConstants(context1, inputs);
}

void DispatchNativeReferencePrepare(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, const NativeReferenceInputs& inputs,
    const NativeReferenceResources& state) noexcept
{
    ScopedNativeReferenceComputeState previous(context, context1);
    std::array<ID3D11ShaderResourceView*, 4> resources{};
    for (std::size_t i = 0; i < resources.size(); ++i) resources[i] = inputs.looks[i].Get();
    context->CSSetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());
    auto* constants = inputs.constants.Get();
    context1->CSSetConstantBuffers1(2, 1, &constants,
        &inputs.firstConstant, &inputs.constantCount);
    auto* output = state.preblendUav.Get();
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(state.preblendShader.Get(), nullptr, 0);
    context->Dispatch(2, 2, 2);
    previous.Restore();
}

} // namespace fo4hdr
