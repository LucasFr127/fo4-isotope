// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace fo4hdr {

struct ArmorEncodedConstants
{
    std::uint32_t encode{};
    float peakUiRelative{1.0F};
    std::array<std::uint32_t, 2> reserved{};
};
static_assert(sizeof(ArmorEncodedConstants) == 16);


struct ArmorCompositionResources
{
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> armorEncodedShader;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> armorEncodedConstants;
    float armorEncodedPeakUiRelative{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> armorEncodedContext1;
    bool armorEncodedAttempted{};
    bool armorEncodedActive{};
    bool armorEncodedFailed{};
    std::uint64_t armorEncodedScope{}, armorEncodedGeneration{}, armorEncodedFrame{};
    std::uint64_t armorEncodedContextGeneration{};
    std::uint32_t armorEncodedThread{};
    const char* armorEncodedStatus{"Off"};
};
extern ArmorCompositionResources g_armor;

void EnsureArmorEncodedResources(ID3D11Device*, ID3D11DeviceContext*) noexcept;
void ResetArmorCompositionResources() noexcept;

} // namespace fo4hdr
