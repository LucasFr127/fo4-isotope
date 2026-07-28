// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderTestOptions.hpp"

namespace fo4hdr::tests::gpu {
namespace {

bool HasArgumentStorage(int argumentCount, const wchar_t* const* arguments) noexcept
{
    if (argumentCount != 2 && argumentCount != 3) return false;
    if (arguments == nullptr) return false;
    if (arguments[0] == nullptr) return false;
    return arguments[0][0] != L'\0';
}

ShaderTestOptions WithBytecodePath(ShaderTestMode mode, const wchar_t* path) noexcept
{
    if (path == nullptr) return {};
    const std::wstring_view value{path};
    if (value.empty() || value.front() == L'-') return {};
    return {mode, value};
}

}  // namespace

ShaderTestOptions ParseShaderTestOptions(
    int argumentCount, const wchar_t* const* arguments) noexcept
{
    if (!HasArgumentStorage(argumentCount, arguments)) return {};
    if (arguments[1] == nullptr) return {};
    if (argumentCount == 2)
        return WithBytecodePath(ShaderTestMode::transfer, arguments[1]);
    if (std::wstring_view{arguments[1]} != L"--gt7-reference") return {};
    return WithBytecodePath(ShaderTestMode::gt7Reference, arguments[2]);
}

}  // namespace fo4hdr::tests::gpu
