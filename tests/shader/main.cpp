// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestOptions.hpp"
#include "ShaderTestSuite.hpp"
#include "TestHarness.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fo4hdr::tests::gpu {

void BenchmarkOperatorLut(const std::filesystem::path& bytecodePath);

namespace {

void RunPresentationCorpus(Device& device)
{
    VerifyContextQuerySafety();
    VerifyPresentationProxy(device);
    VerifyDirectPresentation(device);
    VerifyOutputDitherExperiment(device);
    VerifyPresentationState(device);
    VerifyNativeReferenceState(device);
    VerifyScopedPixelBindingRestoration(device);
}

void RunColorCorpus(Device& device)
{
    g_failures += VerifyOperatorLutSampling(*device.device.Get(), *device.context.Get());
    VerifyShippingPixelShaders(device);
    VerifyNativeVanillaGradeRemoval(device);
    VerifyLutGradeProfiles(device);
    if (!VerifyArmorEncodedComposition(device)) ++g_skips;
    VerifyLayerTransferReference(device);
    VerifyCalibrationPatterns(device);
    VerifySeparatedUiProductionPath(device, TestedToneMapper::aces2Lmt);
    VerifySeparatedUiProductionPath(device, TestedToneMapper::agX);
    VerifyGt7OperatorAndUi(device);
    VerifySeparatedUiProductionPath(device, TestedToneMapper::gt7);
    VerifyGt7OfficialReference(device);
}

void RunDefaultCorpus()
{
    auto device = CreateDevice();
    RunPresentationCorpus(device);
    RunColorCorpus(device);
}

int ReportResult(bool benchmark)
{
    if (g_failures != 0 || fo4hdr::test::failures != 0) return 1;
    if (benchmark)
        std::cout << "PASS isolated WARP operator sampling benchmark (not gameplay cost)\n";
    else {
        std::cout << "PASS aggregate WARP shader corpus";
        if (g_skips != 0) std::cout << " (" << g_skips << " skipped)";
        std::cout << '\n';
    }
    return 0;
}

}  // namespace
}  // namespace fo4hdr::tests::gpu

int wmain(int argumentCount, wchar_t** arguments)
{
    using namespace fo4hdr::tests::gpu;
    try {
        const auto options = ParseShaderTestOptions(argumentCount, arguments);
        if (options.mode == ShaderTestMode::invalid)
            throw std::runtime_error("Usage: fo4hdr-shader-tests [--operator-lut-benchmark <OperatorLut.dxbc>]");
        const bool benchmark = options.mode == ShaderTestMode::operatorLutBenchmark;
        if (benchmark) BenchmarkOperatorLut(std::filesystem::path{options.bytecodePath});
        else RunDefaultCorpus();
        return ReportResult(benchmark);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
