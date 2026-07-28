// SPDX-License-Identifier: GPL-3.0-or-later

#include "../shader/ShaderTestOptions.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <span>
#include <string_view>

namespace fo4hdr::tests {
namespace {

void CheckAcceptedForms()
{
    const wchar_t* transfer[]{L"fo4hdr-shader-tests", L"C:\\shader fixtures\\TransferEvaluation.dxbc"};
    const auto first = gpu::ParseShaderTestOptions(2, transfer);
    FO4HDR_CHECK(first.mode == gpu::ShaderTestMode::transfer);
    FO4HDR_CHECK(first.bytecodePath == transfer[1]);
    FO4HDR_CHECK(first.bytecodePath.data() == transfer[1]);

    const wchar_t* reference[]{L"fo4hdr-shader-tests", L"--gt7-reference", L"Gt7Evaluation.dxbc"};
    const auto second = gpu::ParseShaderTestOptions(3, reference);
    FO4HDR_CHECK(second.mode == gpu::ShaderTestMode::gt7Reference);
    FO4HDR_CHECK(second.bytecodePath == reference[2]);
    FO4HDR_CHECK(second.bytecodePath.data() == reference[2]);
}

void CheckRejected(std::span<const wchar_t* const> arguments)
{
    const auto result = gpu::ParseShaderTestOptions(
        static_cast<int>(arguments.size()), arguments.data());
    FO4HDR_CHECK(result.mode == gpu::ShaderTestMode::invalid);
    FO4HDR_CHECK(result.bytecodePath.empty());
}

void CheckRejectedForms()
{
    const wchar_t* missing[]{L"fo4hdr-shader-tests"};
    const wchar_t* excess[]{L"fo4hdr-shader-tests", L"--gt7-reference", L"Gt7Evaluation.dxbc", L"extra"};
    const wchar_t* flag[]{L"fo4hdr-shader-tests", L"--unsupported-option"};
    const wchar_t* missingReference[]{L"fo4hdr-shader-tests", L"--gt7-reference"};
    const wchar_t* unknown[]{L"fo4hdr-shader-tests", L"--unknown", L"Gt7Evaluation.dxbc"};
    const wchar_t* duplicate[]{L"fo4hdr-shader-tests", L"--gt7-reference", L"--gt7-reference"};
    const wchar_t* emptyPath[]{L"fo4hdr-shader-tests", L""};
    const wchar_t* emptyReference[]{L"fo4hdr-shader-tests", L"--gt7-reference", L""};
    const wchar_t* emptyFlag[]{L"fo4hdr-shader-tests", L"", L"Gt7Evaluation.dxbc"};
    const wchar_t* emptyProgram[]{L"", L"TransferEvaluation.dxbc"};
    const wchar_t* nullPath[]{L"fo4hdr-shader-tests", nullptr};
    const wchar_t* nullReference[]{L"fo4hdr-shader-tests", L"--gt7-reference", nullptr};
    const wchar_t* nullFlag[]{L"fo4hdr-shader-tests", nullptr, L"Gt7Evaluation.dxbc"};
    const wchar_t* nullProgram[]{nullptr, L"TransferEvaluation.dxbc"};
    for (const auto arguments : std::array<std::span<const wchar_t* const>, 14>{
             missing, excess, flag, missingReference, unknown, duplicate,
             emptyPath, emptyReference, emptyFlag, emptyProgram,
             nullPath, nullReference, nullFlag, nullProgram})
        CheckRejected(arguments);
    FO4HDR_CHECK(gpu::ParseShaderTestOptions(2, nullptr).mode == gpu::ShaderTestMode::invalid);
    FO4HDR_CHECK(gpu::ParseShaderTestOptions(0, nullptr).mode == gpu::ShaderTestMode::invalid);
    FO4HDR_CHECK(gpu::ParseShaderTestOptions(-1, nullptr).mode == gpu::ShaderTestMode::invalid);
}

}  // namespace

void RunShaderTestOptionsTests()
{
    CheckAcceptedForms();
    CheckRejectedForms();
}

}  // namespace fo4hdr::tests
