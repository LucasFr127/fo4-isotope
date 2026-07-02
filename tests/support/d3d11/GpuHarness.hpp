// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fo4hdr::tests::gpu {

struct Device
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

void CheckHresult(HRESULT result, std::string_view operation);
[[nodiscard]] Device CreateWarpDevice();
[[nodiscard]] std::vector<char> ReadShader(const std::filesystem::path& path);

struct Float4
{
    float red;
    float green;
    float blue;
    float alpha;
};

inline constexpr std::size_t kTransferWidth = 3;
inline constexpr std::size_t kTransferHeight = 3;
using TransferPixels = std::array<Float4, kTransferWidth * kTransferHeight>;

[[nodiscard]] TransferPixels EvaluateTransfer(const std::filesystem::path& shaderPath,
    const TransferPixels& input, float displayPeakNits);

}  // namespace fo4hdr::tests::gpu
