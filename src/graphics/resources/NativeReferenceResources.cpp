// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "NativeReferenceResources.hpp"

#include <FO4Isotope/ShippingShaders.generated.hpp>
#include <utility>

namespace fo4hdr {
namespace {

[[nodiscard]] bool BuildNativeReferenceShaders(ID3D11Device* device, NativeReferenceResources& resources) noexcept
{
    const auto& pixel = fo4hdr::generated::kNativeReferenceGrade;
    const auto& compute = fo4hdr::generated::kNativeReferencePrepare;
    if (FAILED(device->CreatePixelShader(pixel.data(), pixel.size(), nullptr, &resources.shader)))
        return false;
    return SUCCEEDED(device->CreateComputeShader(compute.data(), compute.size(), nullptr,
        &resources.preblendShader));
}

[[nodiscard]] bool BuildNativeReferenceViews(ID3D11Device* device, NativeReferenceResources& resources) noexcept
{
    if (FAILED(device->CreateShaderResourceView(resources.preblendTexture.Get(), nullptr, &resources.preblendSrv)))
        return false;
    return SUCCEEDED(device->CreateUnorderedAccessView(resources.preblendTexture.Get(), nullptr, &resources.preblendUav));
}

} // namespace

[[nodiscard]] bool BuildNativeReferenceResources(ID3D11Device* device,
    NativeReferenceResources& resources) noexcept
{
    if (!device) return false;
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
    if (!BuildNativeReferenceViews(device, candidate)) return false;
    resources = std::move(candidate);
    return true;
}

void ResetNativeReferenceResources(NativeReferenceResources& resources) noexcept
{
    resources = {};
}

[[nodiscard]] bool NativeReferenceResourcesReady(const NativeReferenceResources& resources) noexcept
{
    return resources.shader && resources.preblendShader &&
        resources.preblendSrv && resources.preblendUav;
}

} // namespace fo4hdr
