// SPDX-License-Identifier: GPL-3.0-or-later

#include "BorrowedComputeState.hpp"

#include <cstddef>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

template <class View, std::size_t Size>
void ReleaseViews(const std::array<View*, Size>& views,
    std::size_t count = Size) noexcept
{
    for (std::size_t slot = 0; slot < count; ++slot)
        if (auto* view = views[slot]) view->Release();
}

constexpr std::array getSrvs_{
    &ID3D11DeviceContext::VSGetShaderResources,
    &ID3D11DeviceContext::HSGetShaderResources,
    &ID3D11DeviceContext::DSGetShaderResources,
    &ID3D11DeviceContext::GSGetShaderResources,
    &ID3D11DeviceContext::PSGetShaderResources,
    &ID3D11DeviceContext::CSGetShaderResources};
constexpr std::array setSrvs_{
    &ID3D11DeviceContext::VSSetShaderResources,
    &ID3D11DeviceContext::HSSetShaderResources,
    &ID3D11DeviceContext::DSSetShaderResources,
    &ID3D11DeviceContext::GSSetShaderResources,
    &ID3D11DeviceContext::PSSetShaderResources,
    &ID3D11DeviceContext::CSSetShaderResources};

} // namespace

BorrowedComputeState::BorrowedComputeState(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1,
    ID3D11Resource* destination) noexcept : context_(context)
{
    if (!context_) return;
    ComPtr<ID3D11Device> device;
    context_->GetDevice(&device);
    uavCount_ = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
        D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    context_->GetPredication(&predicate_, &predicateValue_);
    context_->CSGetShader(&shader_, classes_.data(), &classCount_);
    constantBuffer_.Capture(context_, context1, 0);
    context_->OMGetRenderTargetsAndUnorderedAccessViews(
        static_cast<UINT>(rtvs_.size()), rtvs_.data(), &depth_,
        0, uavCount_, omUavs_.data());
    context_->CSGetUnorderedAccessViews(0, uavCount_, csUavs_.data());

    destinationHasSrvs_ = DestinationHasSrvs(destination);
    csSrvCount_ = destinationHasSrvs_ ? D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT : 4;
    context_->CSGetShaderResources(0, csSrvCount_, srvs_[5].data());
    if (destinationHasSrvs_) {
        for (UINT stage = 0; stage < 5; ++stage)
            (context_->*getSrvs_[stage])(0,
                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_[stage].data());
        // Remove aliases explicitly before binding the destination as a UAV;
        // runtime hazard resolution would otherwise silently discard them.
        for (UINT stage = 0; stage < 6; ++stage)
            UnbindDestinationAliases(stage, destination);
    }
    UnbindOutputs();
}

BorrowedComputeState::~BorrowedComputeState() noexcept
{
    if (!context_) return;
    std::array<ID3D11ShaderResourceView*, 4> emptySrvs{};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> emptyUavs{};
    context_->CSSetShaderResources(0, 4, emptySrvs.data());
    context_->CSSetUnorderedAccessViews(0, uavCount_, emptyUavs.data(), nullptr);
    const auto rtvCount = RenderTargetCount();
    // RTVs and OM UAVs share slot indices. Trim trailing null RTVs so the
    // restored UAV range never overlaps the original render targets.
    std::array<UINT, D3D11_1_UAV_SLOT_COUNT> preserveCounters;
    preserveCounters.fill(UINT(-1));
    if (rtvCount == uavCount_)
        context_->OMSetRenderTargets(rtvCount, rtvs_.data(), depth_.Get());
    else
        context_->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount,
            rtvs_.data(), depth_.Get(), rtvCount, uavCount_ - rtvCount,
            omUavs_.data() + rtvCount, preserveCounters.data());
    context_->CSSetUnorderedAccessViews(0, uavCount_, csUavs_.data(), preserveCounters.data());
    context_->CSSetShaderResources(0, csSrvCount_, srvs_[5].data());
    if (destinationHasSrvs_)
        for (UINT stage = 0; stage < 5; ++stage)
            (context_->*setSrvs_[stage])(0,
                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_[stage].data());
    constantBuffer_.Restore();
    context_->CSSetShader(shader_.Get(), classes_.data(), classCount_);
    context_->SetPredication(predicate_.Get(), predicateValue_);
    ReleaseViews(rtvs_);
    ReleaseViews(omUavs_);
    ReleaseViews(csUavs_);
    ReleaseViews(srvs_[5], csSrvCount_);
    ReleaseGraphicsSrvs();
    ReleaseViews(classes_);
}

void BorrowedComputeState::UnbindOutputs() noexcept
{
    context_->SetPredication(nullptr, FALSE);
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> empty{};
    context_->OMSetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, nullptr, 0, uavCount_, empty.data(), nullptr);
    context_->CSSetUnorderedAccessViews(0, uavCount_, empty.data(), nullptr);
}

bool BorrowedComputeState::DestinationHasSrvs(ID3D11Resource* destination) noexcept
{
    if (!destination) return false;
    D3D11_RESOURCE_DIMENSION dimension{};
    destination->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC description{};
    static_cast<ID3D11Texture2D*>(destination)->GetDesc(&description);
    return (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

UINT BorrowedComputeState::RenderTargetCount() const noexcept
{
    UINT count{};
    for (UINT slot = 0; slot < rtvs_.size(); ++slot)
        if (rtvs_[slot]) count = slot + 1;
    return count;
}

void BorrowedComputeState::ReleaseGraphicsSrvs() noexcept
{
    if (!destinationHasSrvs_) return;
    for (UINT stage = 0; stage < 5; ++stage)
        ReleaseViews(srvs_[stage]);
}

void BorrowedComputeState::UnbindDestinationAliases(UINT stage, ID3D11Resource* destination) noexcept
{
    for (UINT slot = 0; slot < srvs_[stage].size(); ++slot) {
        auto* view = srvs_[stage][slot];
        if (!view) continue;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        if (resource.Get() != destination) continue;
        ID3D11ShaderResourceView* empty{};
        (context_->*setSrvs_[stage])(slot, 1, &empty);
    }
}

} // namespace fo4hdr
