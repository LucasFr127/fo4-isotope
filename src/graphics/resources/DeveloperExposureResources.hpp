// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../DeveloperExposureLock.hpp"
#include "../OperatorConstants.hpp"
#include <d3d11_1.h>
#include <wrl/client.h>

namespace fo4hdr {

struct DeveloperExposureResources
{
    DeveloperExposureLock state;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> captureShader;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    OperatorConstants constantsKey{};
    std::uint64_t observedFrame{};
    bool failed{};
};

extern DeveloperExposureResources g_developerExposure;
void CommitDeveloperExposureAtEndPresent(ID3D11Device*, ID3D11DeviceContext*, bool rendered) noexcept;
void ResetDeveloperExposureResources() noexcept;

} // namespace fo4hdr
