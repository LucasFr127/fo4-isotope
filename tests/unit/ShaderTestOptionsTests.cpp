// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "UnitTestSuite.hpp"
#include "../shader/ShaderTestOptions.hpp"
#include "TestHarness.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fo4hdr::tests {
namespace {

class DiagnosticCapture
{
public:
    DiagnosticCapture() : previous_(std::cerr.rdbuf(text_.rdbuf())) {}
    ~DiagnosticCapture() { std::cerr.rdbuf(previous_); }
    std::string Text() const { return text_.str(); }

private:
    std::ostringstream text_;
    std::streambuf* previous_;
};

bool LegacyDisplayConflict(const gpu::ShaderTestOptions& options)
{
    const bool display = options.presentationBenchmark || options.overlayBenchmark;
    const bool other = options.operatorLutBenchmark || options.armorEncodedMode;
    return display && other;
}

bool LegacyModeConflict(const gpu::ShaderTestOptions& options)
{
    if (options.armorEncodedOnly && options.armorEncodedBenchmark) return true;
    if (options.operatorLutBenchmark && options.armorEncodedMode) return true;
    if (options.presentationBenchmark && options.overlayBenchmark) return true;
    return LegacyDisplayConflict(options);
}

bool LegacySelectionConflict(const gpu::ShaderTestOptions& options, bool benchmark)
{
    return (options.list || options.filtering) && (benchmark || options.armorEncodedMode);
}

std::string_view LegacySelectionError(const gpu::ShaderTestOptions& options, bool benchmark)
{
    if (options.list && options.filtering)
        return "--list and --filter are mutually exclusive.\n";
    if (LegacySelectionConflict(options, benchmark))
        return "Selection and benchmark/armor-only modes are mutually exclusive.\n";
    return {};
}

bool LegacyBenchmark(const gpu::ShaderTestOptions& options)
{
    return options.presentationBenchmark || options.overlayBenchmark ||
        options.operatorLutBenchmark || options.armorEncodedBenchmark;
}

std::string_view LegacyValidationError(const gpu::ShaderTestOptions& options)
{
    const bool benchmark = LegacyBenchmark(options);
    if (LegacyModeConflict(options))
        return "Benchmark and armor-only modes are mutually exclusive.\n";
    if (benchmark && !options.hardware)
        return "Benchmarks require --hardware.\n";
    if (options.debugLayer && benchmark)
        return "Benchmarks require the debug layer disabled.\n";
    return LegacySelectionError(options, benchmark);
}

bool Parse(gpu::ShaderTestOptions& options, std::vector<std::string>& arguments)
{
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    return options.Parse(static_cast<int>(pointers.size()), pointers.data());
}

void CheckFlags(unsigned bits)
{
    gpu::ShaderTestOptions options;
    const std::array destinations{
        &options.hardware, &options.debugLayer, &options.list, &options.filtering,
        &options.armorEncodedOnly, &options.armorEncodedBenchmark,
        &options.operatorLutBenchmark, &options.presentationBenchmark,
        &options.overlayBenchmark};
    for (unsigned index = 0; index < destinations.size(); ++index)
        *destinations[index] = (bits & (1U << index)) != 0;
    options.armorEncodedMode = options.armorEncodedOnly || options.armorEncodedBenchmark;
    const auto expected = LegacyValidationError(options);
    std::string diagnostic;
    bool accepted{};
    {
        DiagnosticCapture capture;
        accepted = options.ValidateModes();
        diagnostic = capture.Text();
    }
    FO4HDR_CHECK(accepted == expected.empty());
    FO4HDR_CHECK(diagnostic == expected);
}

void CheckRecognizedFlags()
{
    const std::array flags{
        "--hardware", "--debug-layer", "--list", "--armor-encoded-only",
        "--armor-encoded-benchmark", "--operator-lut-benchmark",
        "--presentation-benchmark", "--overlay-benchmark"};
    for (unsigned flagIndex = 0; flagIndex < flags.size(); ++flagIndex)
    {
        gpu::ShaderTestOptions options;
        std::vector<std::string> arguments{"shader-tests", flags[flagIndex]};
        FO4HDR_CHECK(Parse(options, arguments));
        const std::array selected{options.hardware, options.debugLayer, options.list,
            options.armorEncodedOnly, options.armorEncodedBenchmark,
            options.operatorLutBenchmark, options.presentationBenchmark,
            options.overlayBenchmark};
        for (unsigned index = 0; index < selected.size(); ++index)
            FO4HDR_CHECK(selected[index] == (index == flagIndex));
        FO4HDR_CHECK(!options.filtering);
        FO4HDR_CHECK(options.filter.empty());
        FO4HDR_CHECK(options.armorEncodedMode ==
            (options.armorEncodedOnly || options.armorEncodedBenchmark));
    }
    gpu::ShaderTestOptions options;
    std::vector<std::string> arguments{"shader-tests", "--hardware", "--debug-layer"};
    FO4HDR_CHECK(Parse(options, arguments));
    FO4HDR_CHECK(options.hardware && options.debugLayer);
}

void CheckFilterParsing()
{
    gpu::ShaderTestOptions options;
    std::array arguments{"shader-tests", "--filter", "operator-lut", "--filter", ""};
    std::array<char*, arguments.size()> pointers{};
    for (std::size_t index = 0; index < pointers.size(); ++index)
        pointers[index] = const_cast<char*>(arguments[index]);
    FO4HDR_CHECK(options.Parse(static_cast<int>(pointers.size()), pointers.data()));
    FO4HDR_CHECK(options.filtering);
    FO4HDR_CHECK(options.filter.empty());
    FO4HDR_CHECK(options.ValidateModes());
}

void CheckRejectedArguments()
{
    const std::array invalid{
        std::vector<std::string>{"shader-tests", "--filter"},
        std::vector<std::string>{"shader-tests", "--unknown"}};
    for (auto arguments : invalid)
    {
        gpu::ShaderTestOptions options;
        std::string diagnostic;
        bool accepted{};
        {
            DiagnosticCapture capture;
            accepted = Parse(options, arguments);
            diagnostic = capture.Text();
        }
        FO4HDR_CHECK(!accepted);
        FO4HDR_CHECK(diagnostic.starts_with("Usage: fo4hdr-shader-tests "));
    }
}

void CheckRuntimeSelection()
{
    for (const auto* flag : {"--ae-runtime", "--ng-runtime"})
    {
        gpu::ShaderTestOptions options;
        std::vector<std::string> arguments{"shader-tests", flag};
        FO4HDR_CHECK(Parse(options, arguments));
        FO4HDR_CHECK(options.aeRuntime != options.ngRuntime);
        FO4HDR_CHECK(options.ValidateModes());
    }
    gpu::ShaderTestOptions options;
    std::vector<std::string> arguments{"shader-tests", "--ae-runtime", "--ng-runtime"};
    FO4HDR_CHECK(Parse(options, arguments));
    DiagnosticCapture capture;
    FO4HDR_CHECK(!options.ValidateModes());
    FO4HDR_CHECK(capture.Text() == "AE and NG runtime selections are mutually exclusive.\n");
}

}  // namespace

void RunShaderTestOptionsTests()
{
    for (unsigned bits = 0; bits < 512; ++bits) CheckFlags(bits);
    CheckRecognizedFlags();
    CheckFilterParsing();
    CheckRejectedArguments();
    CheckRuntimeSelection();
}

}  // namespace fo4hdr::tests
