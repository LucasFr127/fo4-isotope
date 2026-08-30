// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeReferenceResources.hpp"

#include <fo4hdr-shaders/NativeReferencePrepare.hpp>

#include <utility>

namespace fo4hdr {
namespace {

[[nodiscard]] bool BuildNativeReferenceShaders(ID3D11Device* device, NativeReferenceResources& resources) noexcept
{
    return SUCCEEDED(device->CreateComputeShader(kNativeReferencePrepareBytecode,
        sizeof(kNativeReferencePrepareBytecode), nullptr, &resources.preblendShader)) &&
        resources.preblendShader;
}

[[nodiscard]] bool BuildNativeReferenceViews(ID3D11Device* device, NativeReferenceResources& resources) noexcept
{
    if (FAILED(device->CreateShaderResourceView(resources.preblendTexture.Get(), nullptr, &resources.preblendSrv)))
        return false;
    return resources.preblendSrv &&
        SUCCEEDED(device->CreateUnorderedAccessView(resources.preblendTexture.Get(), nullptr, &resources.preblendUav)) &&
        resources.preblendUav;
}

[[nodiscard]] bool SupportsPreparedLook(ID3D11Device* device) noexcept
{
    UINT support{};
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE3D | D3D11_FORMAT_SUPPORT_SHADER_LOAD |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT, &support))) return false;
    if ((support & required) != required) return false;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 details{DXGI_FORMAT_R32G32B32A32_FLOAT, 0};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &details, sizeof(details))))
        return false;
    return (details.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

} // namespace

[[nodiscard]] bool BuildNativeReferenceResources(ID3D11Device* device,
    NativeReferenceResources& resources) noexcept
{
    if (!device || !SupportsPreparedLook(device)) return false;
    NativeReferenceResources candidate;
    D3D11_TEXTURE3D_DESC description{};
    description.Width = 20;
    description.Height = description.Depth = 16;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (!BuildNativeReferenceShaders(device, candidate)) return false;
    if (FAILED(device->CreateTexture3D(&description, nullptr, &candidate.preblendTexture)))
        return false;
    if (!candidate.preblendTexture || !BuildNativeReferenceViews(device, candidate)) return false;
    resources = std::move(candidate);
    return true;
}

void ResetNativeReferenceResources(NativeReferenceResources& resources) noexcept
{
    resources = {};
}

[[nodiscard]] bool NativeReferenceResourcesReady(const NativeReferenceResources& resources) noexcept
{
    return resources.preblendTexture && resources.preblendShader &&
        resources.preblendSrv && resources.preblendUav;
}

} // namespace fo4hdr
