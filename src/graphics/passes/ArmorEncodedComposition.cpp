// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ArmorEncodedComposition.hpp"

#include "../d3d11/ScopedD3D11ComputeConstantBuffer.hpp"

#include <wrl/client.h>

#include <array>
#include <cstddef>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

template <class Interface, std::size_t Size>
void ReleaseArmorCapturedViews(const std::array<Interface*, Size>& views) noexcept
{
    for (auto* view : views)
        if (view) view->Release();
}

bool ReleaseArmorUavs(
    const std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT>& views) noexcept
{
    bool occupied{};
    for (auto* view : views) {
        if (!view) continue;
        occupied = true;
        view->Release();
    }
    return occupied;
}

struct ArmorEncodedComputeState
{
    ID3D11DeviceContext* context;
    ArmorEncodedSetRenderTargets setRenderTargets;
    std::array<std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 6> srvs{};
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> classes{};
    UINT classCount{D3D11_SHADER_MAX_INTERFACES};
    fo4hdr::ScopedD3D11ComputeConstantBuffer constants;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue{};
    bool mutated{};
    ~ArmorEncodedComputeState() noexcept
    {
        if (mutated)
        {
            ID3D11UnorderedAccessView* nullUav{};
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            context->CSSetShader(shader.Get(), classes.data(), classCount);
            constants.Restore();
            SetTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), depth.Get());
            context->VSSetShaderResources(0, 128, srvs[0].data());
            context->HSSetShaderResources(0, 128, srvs[1].data());
            context->DSSetShaderResources(0, 128, srvs[2].data());
            context->GSSetShaderResources(0, 128, srvs[3].data());
            context->PSSetShaderResources(0, 128, srvs[4].data());
            context->CSSetShaderResources(0, 128, srvs[5].data());
            context->SetPredication(predicate.Get(), predicateValue);
        }
        for (auto& stage : srvs) ReleaseArmorCapturedViews(stage);
        ReleaseArmorCapturedViews(rtvs);
        ReleaseArmorCapturedViews(classes);
    }
    void SetTargets(UINT targetCount, ID3D11RenderTargetView* const* targets,
        ID3D11DepthStencilView* depthView) noexcept
    {
        // Production uses its captured next setter, preserving logical
        // presentation-target metadata and avoiding observer side effects.
        if (setRenderTargets) setRenderTargets(context, targetCount, targets, depthView);
        else context->OMSetRenderTargets(targetCount, targets, depthView);
    }
};

} // namespace

[[nodiscard]] bool DispatchArmorEncodedComposition(
    ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
    const ArmorEncodedDispatch& dispatch,
    ArmorEncodedSetRenderTargets setRenderTargets) noexcept
{
    const auto& [shader, output, constants, width, height] = dispatch;
    if (!context || !context1) return false;
    if (!dispatch.Ready()) return false;
    // Bound UAVs may carry hidden append/counter state. Reject them before
    // mutation rather than approximating their counter or binding restoration.
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const UINT uavCount = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
        D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> uavs{};
    context->CSGetUnorderedAccessViews(0, uavCount, uavs.data());
    bool occupied = ReleaseArmorUavs(uavs);
    uavs.fill(nullptr);
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
        0, uavCount, uavs.data());
    occupied |= ReleaseArmorUavs(uavs);
    if (occupied) return false;

    ArmorEncodedComputeState state{context, setRenderTargets};
    context->VSGetShaderResources(0, 128, state.srvs[0].data());
    context->HSGetShaderResources(0, 128, state.srvs[1].data());
    context->DSGetShaderResources(0, 128, state.srvs[2].data());
    context->GSGetShaderResources(0, 128, state.srvs[3].data());
    context->PSGetShaderResources(0, 128, state.srvs[4].data());
    context->CSGetShaderResources(0, 128, state.srvs[5].data());
    context->OMGetRenderTargets(8, state.rtvs.data(), &state.depth);
    context->CSGetShader(&state.shader, state.classes.data(), &state.classCount);
    state.constants.Capture(context, context1, 13);
    context->GetPredication(&state.predicate, &state.predicateValue);
    state.mutated = true;
    std::array<ID3D11ShaderResourceView*, 128> empty{};
    context->VSSetShaderResources(0, 128, empty.data());
    context->HSSetShaderResources(0, 128, empty.data());
    context->DSSetShaderResources(0, 128, empty.data());
    context->GSSetShaderResources(0, 128, empty.data());
    context->PSSetShaderResources(0, 128, empty.data());
    context->CSSetShaderResources(0, 128, empty.data());
    state.SetTargets(0, nullptr, nullptr);
    context->SetPredication(nullptr, FALSE);
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(13, 1, &constants);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

} // namespace fo4hdr
