// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestOptions.hpp"
#include "d3d11/GpuHarness.hpp"
#include "ShaderTestSuite.hpp"
#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <string_view>
#include <tuple>

namespace fo4hdr::tests::gpu {

constexpr std::array<std::string_view, 17> kSectionNames{
    "operator-lut",
    "context-query-safety",
    "presentation-proxy",
    "presentation-state",
    "direct-presentation",
    "output-dither",
    "scoped-pixel-binding-restoration",
    "shipping-pixel-shaders",
    "lut-grade-profiles",
    "armor-encoded-composition",
    "layer-transfer-reference",
    "calibration-patterns",
    "separated-ui-aces2-lmt",
    "separated-ui-agx",
    "gt7-operator-ui",
    "separated-ui-gt7",
    "gt7-official-reference",
};

const char* SectionOutcome(int failuresBefore, int skipsBefore)
{
    if (g_failures != failuresBefore) return "failed";
    if (g_skips != skipsBefore) return "skipped";
    return "passed";
}

template <class Function>
bool RunSection(
    std::string_view filter,
    std::string_view name,
    Function&& function)
{
    if (!filter.empty() && filter != name) return false;
    const auto failuresBefore = g_failures;
    const auto skipsBefore = g_skips;
    std::cout << "[ RUN ] " << name << std::endl;
    try {
        function();
    } catch (...) {
        std::cerr << "[ END ] " << name << " failed" << std::endl;
        throw;
    }
    std::cout << "[ END ] " << name << ' '
              << SectionOutcome(failuresBefore, skipsBefore) << '\n';
    return true;
}

struct ShippingShaderRun
{
    ShaderTestOptions options;
    bool armorEncodedRan{};
    bool KnownFilter() const
    {
        return !options.filtering || std::find(kSectionNames.begin(), kSectionNames.end(), options.filter) != kSectionNames.end();
    }
    void RunDefault(Device& device)
    {
        RunSection(options.filter, "operator-lut", [&] {
            g_failures += VerifyOperatorLutSampling(
                *device.device.Get(), *device.context.Get(), false);
        });
        RunSection(options.filter, "context-query-safety", VerifyContextQuerySafety);
        RunSection(options.filter, "presentation-proxy", [&] {
            VerifyPresentationProxy(device);
        });
        RunSection(options.filter, "direct-presentation", [&] { VerifyDirectPresentation(device); });
        RunSection(options.filter, "output-dither", [&] { VerifyOutputDitherExperiment(device); });
        RunSection(options.filter, "presentation-state", [&] {
            VerifyPresentationState(device);
            VerifyNativeReferenceState(device);
        });
        RunSection(options.filter, "scoped-pixel-binding-restoration", [&] {
            VerifyScopedPixelBindingRestoration(device);
        });
        RunSection(options.filter, "shipping-pixel-shaders", [&] {
            VerifyShippingPixelShaders(device);
            VerifyNativeVanillaGradeRemoval(device);
        });
        if (options.filter == "lut-grade-profiles")
            RunSection(options.filter, "lut-grade-profiles", [&] {
                VerifyLutGradeProfiles(device);
            });
        RunSection(options.filter, "armor-encoded-composition", [&] {
            armorEncodedRan = VerifyArmorEncodedComposition(device);
            if (!armorEncodedRan) ++g_skips;
        });
        RunSection(options.filter, "layer-transfer-reference", [&] {
            VerifyLayerTransferReference(device);
        });
        RunSection(options.filter, "calibration-patterns", [&] {
            VerifyCalibrationPatterns(device);
        });
        RunSection(options.filter, "separated-ui-aces2-lmt", [&] {
            VerifySeparatedUiProductionPath(
                device, TestedToneMapper::aces2Lmt);
        });
        RunSection(options.filter, "separated-ui-agx", [&] {
            VerifySeparatedUiProductionPath(device, TestedToneMapper::agX);
        });
        RunSection(options.filter, "gt7-operator-ui", [&] {
            VerifyGt7OperatorAndUi(device);
        });
        RunSection(options.filter, "separated-ui-gt7", [&] {
            VerifySeparatedUiProductionPath(device, TestedToneMapper::gt7);
        });
        RunSection(options.filter, "gt7-official-reference", [&] {
            VerifyGt7OfficialReference(device);
        });
    }
    void Execute()
    {
        std::cout << "[ SUITE START ] shipping-shaders" << std::endl;
        try {
            auto device = CreateDevice(options.hardware, options.debugLayer);
            RunSelected(device);
            if (options.debugLayer) CheckDebugMessages(device);
        } catch (const std::exception& error) {
            ++g_failures;
            std::cerr << "shader execution failed: "
                      << error.what() << '\n';
        }
    }
    void RunSelected(Device& device)
    {
        if (options.presentationBenchmark || options.overlayBenchmark)
        {
            RunSection({}, "direct-presentation", [&] { BenchmarkDirectPresentation(device, options.overlayBenchmark); });
        }
        else if (options.armorEncodedMode)
        {
            RunSection({}, "armor-encoded-composition", [&] {
                armorEncodedRan = VerifyArmorEncodedComposition(
                    device, options.armorEncodedBenchmark);
                if (!armorEncodedRan) ++g_skips;
            });
        }
        else if (options.operatorLutBenchmark)
        {
            RunSection({}, "operator-lut", [&] {
                g_failures += VerifyOperatorLutSampling(
                    *device.device.Get(), *device.context.Get(), true);
            });
        }
        else
        {
            RunDefault(device);
        }
    }
    int Report()
    {
        if (g_failures != 0) {
            std::cout << "[ SUITE END ] shipping-shaders failed\n";
            std::cerr << g_failures << " shipping shader test(s) failed.\n";
            return 1;
        }
        return ReportCompletedExecution();
    }
    int ReportCompletedExecution()
    {
        if (options.presentationBenchmark)
        {
            return ReportBenchmark("Presentation mechanism");
        }
        if (options.armorEncodedMode)
        {
            return ReportArmor();
        }
        if (options.filtering && g_skips != 0)
        {
            std::cout << "[ SUITE END ] shipping-shaders skipped\n";
            return 77;
        }
        if (options.operatorLutBenchmark)
        {
            return ReportBenchmark("Operator LUT");
        }
        return ReportSuccess();
    }
    int ReportBenchmark(std::string_view name) const
    {
        std::cout << name << " benchmark completed on D3D11 hardware.\n";
        std::cout << "[ SUITE END ] shipping-shaders passed\n";
        return 0;
    }
    const char* AdapterName() const
    {
        return options.hardware ? "D3D11 hardware." : "D3D11 WARP.";
    }
    int ReportArmor()
    {
        const auto [description, outcome, status] = armorEncodedRan
            ? std::tuple{"passed on ", "passed", 0}
            : std::tuple{"unavailable on ", "skipped", 77};
        std::cout << "Armor encoded composition " << description << AdapterName() << '\n';
        std::cout << "[ SUITE END ] shipping-shaders " << outcome << '\n';
        return status;  // Conventional test-harness skip status.
    }
    int ReportSuccess()
    {
        std::cout << "[ SUITE END ] shipping-shaders passed";
        if (g_skips != 0) std::cout << " (" << g_skips << " skipped)";
        std::cout << '\n';
        if (options.filtering) {
            std::cout << "Selected shipping shader test section passed on "
                      << AdapterName() << '\n';
            return 0;
        }
        std::cout << "shipping HDR10, ACES 2 LMT, "
                     "AgX, GT7, CreativeGradeLut, UI, Video, FXAA, "
                     "and both temporal variants passed on "
                  << AdapterName() << '\n';
        return 0;
    }
};

}  // namespace fo4hdr::tests::gpu

int main(int argumentCount, char** arguments)
{
    using namespace fo4hdr::tests::gpu;
    ShippingShaderRun run;
    if (!run.options.Parse(argumentCount, arguments) || !run.options.ValidateModes()) return 2;
    if (run.options.list) {
        for (const auto name : kSectionNames) std::cout << name << '\n';
        return 0;
    }
    if (!run.KnownFilter()) {
        std::cerr << "Unknown test section: " << run.options.filter << '\n';
        return 2;
    }
    run.Execute();
    return run.Report();
}
