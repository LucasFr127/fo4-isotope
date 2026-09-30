// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "ShaderPixels.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace fo4hdr::tests::gpu {

[[nodiscard]] std::uint16_t FloatToBinary16(float value) noexcept;
[[nodiscard]] float Binary16ToFloat(std::uint16_t bits) noexcept;

enum class TestedToneMapper
{
    aces2,
    agX,
    gt7
};

[[nodiscard]] std::array<std::array<std::uint16_t, 4>, kWidth>
MakeFp16Pixels(const std::array<Float4, kWidth>& samples);

[[nodiscard]] std::array<std::array<std::uint8_t, 4>, kWidth>
MakeUnorm8Pixels(const std::array<Float4, kWidth>& samples);

[[nodiscard]] unsigned CodeDistance(
    std::uint16_t left,
    std::uint16_t right) noexcept;

[[nodiscard]] double Binary16LargestAdjacentStep(float value) noexcept;

[[nodiscard]] double Gt7Bt2020ErrorBound(
    fo4hdr::LinearRgb expectedNits,
    std::size_t channel) noexcept;

[[nodiscard]] fo4hdr::LinearRgb MapProductionOperator(
    TestedToneMapper toneMapper,
    fo4hdr::LinearRgb scene,
    float displayPeakNits) noexcept;

[[nodiscard]] fo4hdr::ProductionToneMapperAlgorithm ProductionAlgorithm(
    TestedToneMapper toneMapper) noexcept;

[[nodiscard]] std::uint32_t ProductionMode(
    TestedToneMapper toneMapper) noexcept;

[[nodiscard]] double DecodeSrgb(double encoded) noexcept;

[[nodiscard]] unsigned TemporalHistoryCode(double encoded) noexcept;

[[nodiscard]] std::array<Float4, kWidth> UniformPixels(Float4 value) noexcept;

}  // namespace fo4hdr::tests::gpu
