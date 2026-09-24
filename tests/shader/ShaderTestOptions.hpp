// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace fo4hdr::tests::gpu {

enum class ShaderTestMode : std::uint8_t
{
    invalid,
    defaultCorpus,
    operatorLutBenchmark
};

struct ShaderTestOptions
{
    ShaderTestMode mode{ShaderTestMode::invalid};
    // Borrows the original wmain argument storage until the selected run returns.
    std::wstring_view bytecodePath;
};

[[nodiscard]] ShaderTestOptions ParseShaderTestOptions(
    int argumentCount, const wchar_t* const* arguments) noexcept;

}  // namespace fo4hdr::tests::gpu
