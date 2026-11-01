// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "AffineResources.hpp"

#include <FO4Isotope/ShippingShaders.generated.hpp>
#include <utility>

namespace fo4hdr {
namespace {

[[nodiscard]] bool BuildAffineShaders(ID3D11Device* device, AffineResources& resources) noexcept
{
    const auto& pixel = fo4hdr::generated::kAffineGrade;
    const auto& compute = fo4hdr::generated::kAffinePrepare;
    if (FAILED(device->CreatePixelShader(pixel.data(), pixel.size(), nullptr, &resources.shader)))
        return false;
    return SUCCEEDED(device->CreateComputeShader(compute.data(), compute.size(), nullptr,
        &resources.preblendShader));
}

[[nodiscard]] bool BuildAffineViews(ID3D11Device* device, AffineResources& resources) noexcept
{
    if (FAILED(device->CreateShaderResourceView(resources.preblendTexture.Get(), nullptr, &resources.preblendSrv)))
        return false;
    return SUCCEEDED(device->CreateUnorderedAccessView(resources.preblendTexture.Get(), nullptr, &resources.preblendUav));
}

} // namespace

[[nodiscard]] bool BuildAffineResources(ID3D11Device* device,
    AffineResources& resources) noexcept
{
    if (!device) return false;
    AffineResources candidate;
    D3D11_TEXTURE3D_DESC description{};
    description.Width = 20;
    description.Height = description.Depth = 16;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (!BuildAffineShaders(device, candidate)) return false;
    if (FAILED(device->CreateTexture3D(&description, nullptr, &candidate.preblendTexture)))
        return false;
    if (!BuildAffineViews(device, candidate)) return false;
    resources = std::move(candidate);
    return true;
}

void ResetAffineResources(AffineResources& resources) noexcept
{
    resources = {};
}

[[nodiscard]] bool AffineResourcesReady(const AffineResources& resources) noexcept
{
    return resources.shader && resources.preblendShader &&
        resources.preblendSrv && resources.preblendUav;
}

} // namespace fo4hdr
