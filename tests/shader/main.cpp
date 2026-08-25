// SPDX-License-Identifier: GPL-3.0-or-later

#include "d3d11/GpuHarness.hpp"
#include "ShaderTestSuite.hpp"
#include "ShaderTestOptions.hpp"
#include "Gt7ReferenceTests.hpp"
#include "UnitTestSuite.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using fo4hdr::tests::gpu::TransferPixels;
constexpr float displayPeakNits = 400.0f;
constexpr double maximumEncodedError = 1.0e-5;

TransferPixels TransferInputs()
{
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    return {{{0.0f, 0.0f, 0.0f, 0.0f},
        {0.001f, 0.001f, 0.001f, 0.0f},
        {203.0f, 203.0f, 203.0f, 0.0f},
        {300.0f, 300.0f, 300.0f, 0.0f},
        {401.0f, 401.0f, 401.0f, 0.0f},
        {-1.0f, -infinity, -10.0f, 0.0f},
        {nan, nan, nan, 0.0f},
        {infinity, infinity, infinity, 0.0f},
        {0.001f, 203.0f, 401.0f, 0.0f}}};
}

void CheckChannel(float actual, double expected, double& observedError)
{
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(actual >= 0.0f && actual <= 1.0f);
    const auto error = std::abs(static_cast<double>(actual) - expected);
    FO4HDR_CHECK(error <= maximumEncodedError);
    observedError = std::isfinite(error) ? std::max(observedError, error) :
        std::numeric_limits<double>::infinity();
}

double CheckTransfer(const TransferPixels& input, const TransferPixels& output)
{
    double observedError{};
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto& value = input[index];
        const auto expected = fo4hdr::EncodeSt2084Pq(fo4hdr::ContainBt2020Nits(
            {value.red, value.green, value.blue}, displayPeakNits));
        CheckChannel(output[index].red, expected.red, observedError);
        CheckChannel(output[index].green, expected.green, observedError);
        CheckChannel(output[index].blue, expected.blue, observedError);
        FO4HDR_CHECK(output[index].alpha == 1.0f);
    }
    for (std::size_t index = 1; index <= 4; ++index)
    {
        FO4HDR_CHECK(output[index].red >= output[index - 1].red);
        FO4HDR_CHECK(output[index].green >= output[index - 1].green);
        FO4HDR_CHECK(output[index].blue >= output[index - 1].blue);
    }
    FO4HDR_CHECK(output[3].red > output[2].red);
    FO4HDR_CHECK(output[3].green > output[2].green);
    FO4HDR_CHECK(output[3].blue > output[2].blue);
    return observedError;
}

int RunGt7Reference(const std::filesystem::path& shaderPath)
{
    fo4hdr::tests::RunGt7Tests();
    const auto error = fo4hdr::tests::gpu::VerifyGt7Reference(shaderPath);
    if (fo4hdr::test::failures != 0) return EXIT_FAILURE;
    std::cout << "PASS GT7 CPU/source and WARP raw-reference; maximum framebuffer error "
              << error << '\n';
    return EXIT_SUCCESS;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        const auto options = fo4hdr::tests::gpu::ParseShaderTestOptions(argc, argv);
        if (options.mode == fo4hdr::tests::gpu::ShaderTestMode::gt7Reference)
            return RunGt7Reference(std::filesystem::path{options.bytecodePath});
        if (options.mode == fo4hdr::tests::gpu::ShaderTestMode::invalid)
        {
            throw std::runtime_error("Usage: fo4hdr-shader-tests <TransferEvaluation.dxbc> | --gt7-reference <Gt7Evaluation.dxbc>");
        }
        const std::filesystem::path shaderPath{options.bytecodePath};
        const auto input = TransferInputs();
        const auto output = fo4hdr::tests::gpu::EvaluateTransfer(shaderPath, input, displayPeakNits);
        const auto error = CheckTransfer(input, output);
        fo4hdr::tests::gpu::VerifyShippingPixelShaders(
            shaderPath.parent_path());
        fo4hdr::tests::gpu::VerifyImageControls(shaderPath.parent_path());
        fo4hdr::tests::gpu::VerifyArmorEncodedComposition(shaderPath.parent_path());
        fo4hdr::tests::gpu::VerifyCreativeGradeLut(shaderPath.parent_path());
        fo4hdr::tests::gpu::VerifyHdrGradeTransfer(shaderPath.parent_path() / "HdrGradeTransfer.dxbc");
        fo4hdr::tests::gpu::VerifyOperatorLut(
            shaderPath.parent_path());
        fo4hdr::tests::gpu::VerifyDeveloperExposure(
            shaderPath.parent_path());
        fo4hdr::tests::RunSettingsPanelTests();
        if (fo4hdr::test::failures != 0)
        {
            return EXIT_FAILURE;
        }
        std::cout << "PASS WARP transfer/containment; maximum encoded error " << error << '\n';
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
