// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestOptions.hpp"

namespace fo4hdr::tests::gpu {
namespace {

bool HasArgumentStorage(int argumentCount, const wchar_t* const* arguments) noexcept
{
    if (argumentCount != 1 && argumentCount != 3) return false;
    if (arguments == nullptr) return false;
    if (arguments[0] == nullptr) return false;
    return arguments[0][0] != L'\0';
}

ShaderTestOptions WithBenchmarkPath(const wchar_t* value) noexcept
{
    if (value == nullptr) return {};
    const std::wstring_view path{value};
    if (path.empty() || path.front() == L'-') return {};
    return {ShaderTestMode::operatorLutBenchmark, path};
}

}  // namespace

ShaderTestOptions ParseShaderTestOptions(
    int argumentCount, const wchar_t* const* arguments) noexcept
{
    if (!HasArgumentStorage(argumentCount, arguments)) return {};
    if (argumentCount == 1) return {ShaderTestMode::defaultCorpus, {}};
    if (arguments[1] == nullptr) return {};
    if (std::wstring_view{arguments[1]} != L"--operator-lut-benchmark") return {};
    return WithBenchmarkPath(arguments[2]);
}

}  // namespace fo4hdr::tests::gpu
