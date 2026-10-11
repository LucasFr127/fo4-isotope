// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ArmorCompositionResources.hpp"
#include "../LiveSettings.hpp"
#include <FO4Isotope/ShippingShaders.generated.hpp>
#include <utility>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

ArmorCompositionResources g_armor;

[[nodiscard]] bool CreateArmorConstantBuffers(ID3D11Device* device,
    std::array<ComPtr<ID3D11Buffer>, 2>& buffers) noexcept
{
    struct BufferConfiguration
    {
        D3D11_USAGE usage;
        UINT cpuAccess;
        ArmorEncodedConstants values;
    };
    const auto sharedSrgb = static_cast<std::uint32_t>(g_configuration.productionRequested);
    const std::array<BufferConfiguration, 2> configurations{{
        {D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, {0, 1.0F, sharedSrgb, 0}},
        {D3D11_USAGE_IMMUTABLE, 0, {1, 0.0F, sharedSrgb, 0}}
    }};
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(ArmorEncodedConstants);
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (std::size_t i = 0; i < configurations.size(); ++i) {
        const auto& configuration = configurations[i];
        description.Usage = configuration.usage;
        description.CPUAccessFlags = configuration.cpuAccess;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = &configuration.values;
        if (FAILED(device->CreateBuffer(&description, &initial, &buffers[i]))) return false;
    }
    return true;
}

void EnsureArmorEncodedResources(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (g_armor.armorEncodedAttempted) return;
    if (!device || !context) return;
    g_armor.armorEncodedAttempted = true;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11DeviceContext1> context1;
    std::array<ComPtr<ID3D11Buffer>, 2> buffers;
    const auto& code = generated::kArmorEncodedCompositionComputeShader;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return;
    if (FAILED(device->CreateComputeShader(code.data(), code.size(), nullptr, &shader))) return;
    if (!CreateArmorConstantBuffers(device, buffers)) return;
    g_armor.armorEncodedShader = std::move(shader);
    g_armor.armorEncodedContext1 = std::move(context1);
    g_armor.armorEncodedConstants = std::move(buffers);
    g_armor.armorEncodedSharedSrgb = g_configuration.productionRequested;
}

void ResetArmorCompositionResources() noexcept
{
    g_armor.armorEncodedShader.Reset();
    for (auto& buffer : g_armor.armorEncodedConstants) buffer.Reset();
    g_armor.armorEncodedPeakUiRelative = 0.0F;
    g_armor.armorEncodedWhite = 0.0F;
    g_armor.armorEncodedResource = 0;
    g_armor.armorEncodedContext1.Reset();
    g_armor.armorEncodedAttempted = false;
    g_armor.armorEncodedActive = false;
    g_armor.armorEncodedFailed = false;
    g_armor.armorEncodedSharedSrgb = false;
    g_armor.armorEncodedStatus = "Waiting for qualified post-AA proxy";
}

} // namespace fo4hdr
