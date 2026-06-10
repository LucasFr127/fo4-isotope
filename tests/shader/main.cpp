// SPDX-License-Identifier: GPL-3.0-or-later

#include "d3d11/GpuHarness.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
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

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 2)
        {
            throw std::runtime_error("Usage: fo4hdr-shader-tests <TransferEvaluation.dxbc>");
        }
        const auto input = TransferInputs();
        const auto output = fo4hdr::tests::gpu::EvaluateTransfer(argv[1], input, displayPeakNits);
        const auto error = CheckTransfer(input, output);
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
