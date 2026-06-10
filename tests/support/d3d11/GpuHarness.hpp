// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

namespace fo4hdr::tests::gpu {

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
