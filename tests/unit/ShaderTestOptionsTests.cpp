// SPDX-License-Identifier: GPL-3.0-or-later

#include "../shader/ShaderTestOptions.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <span>

namespace fo4hdr::tests {
namespace {

void CheckAcceptedForms()
{
    const wchar_t* defaults[]{L"fo4hdr-shader-tests"};
    const auto corpus = gpu::ParseShaderTestOptions(1, defaults);
    FO4HDR_CHECK(corpus.mode == gpu::ShaderTestMode::defaultCorpus);
    FO4HDR_CHECK(corpus.bytecodePath.empty());

    const wchar_t* benchmark[]{L"fo4hdr-shader-tests", L"--operator-lut-benchmark", L"C:\\shader fixtures\\OperatorLut.dxbc"};
    const auto selected = gpu::ParseShaderTestOptions(3, benchmark);
    FO4HDR_CHECK(selected.mode == gpu::ShaderTestMode::operatorLutBenchmark);
    FO4HDR_CHECK(selected.bytecodePath == benchmark[2]);
    FO4HDR_CHECK(selected.bytecodePath.data() == benchmark[2]);
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
    const wchar_t* transfer[]{L"fo4hdr-shader-tests", L"TransferEvaluation.dxbc"};
    const wchar_t* reference[]{L"fo4hdr-shader-tests", L"--gt7-reference", L"Gt7Evaluation.dxbc"};
    const wchar_t* excess[]{L"fo4hdr-shader-tests", L"--operator-lut-benchmark", L"OperatorLut.dxbc", L"extra"};
    const wchar_t* missingPath[]{L"fo4hdr-shader-tests", L"--operator-lut-benchmark"};
    const wchar_t* unknown[]{L"fo4hdr-shader-tests", L"--unsupported-option", L"OperatorLut.dxbc"};
    const wchar_t* duplicate[]{L"fo4hdr-shader-tests", L"--operator-lut-benchmark", L"--operator-lut-benchmark"};
    const wchar_t* emptyPath[]{L"fo4hdr-shader-tests", L"--operator-lut-benchmark", L""};
    const wchar_t* emptyFlag[]{L"fo4hdr-shader-tests", L"", L"OperatorLut.dxbc"};
    const wchar_t* emptyProgram[]{L""};
    const wchar_t* nullPath[]{L"fo4hdr-shader-tests", L"--operator-lut-benchmark", nullptr};
    const wchar_t* nullFlag[]{L"fo4hdr-shader-tests", nullptr, L"OperatorLut.dxbc"};
    const wchar_t* nullProgram[]{nullptr};
    for (const auto arguments : std::array<std::span<const wchar_t* const>, 12>{
             transfer, reference, excess, missingPath, unknown, duplicate,
             emptyPath, emptyFlag, emptyProgram, nullPath, nullFlag, nullProgram})
        CheckRejected(arguments);
    FO4HDR_CHECK(gpu::ParseShaderTestOptions(1, nullptr).mode == gpu::ShaderTestMode::invalid);
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
