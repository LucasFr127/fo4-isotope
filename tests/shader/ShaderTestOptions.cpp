// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestOptions.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <utility>

namespace fo4hdr::tests::gpu {

bool ShaderTestOptions::SetFlag(std::string_view argument)
{
    const std::pair<std::string_view, bool*> flags[]{
        {"--hardware", &hardware}, {"--debug-layer", &debugLayer}, {"--ae-runtime", &aeRuntime},
        {"--ng-runtime", &ngRuntime},
        {"--list", &list}, {"--armor-encoded-only", &armorEncodedOnly},
        {"--armor-encoded-benchmark", &armorEncodedBenchmark},
        {"--operator-lut-benchmark", &operatorLutBenchmark},
        {"--presentation-benchmark", &presentationBenchmark},
        {"--overlay-benchmark", &overlayBenchmark}};
    const auto selected = std::ranges::find(flags, argument,
        &std::pair<std::string_view, bool*>::first);
    if (selected == std::end(flags)) return false;
    *selected->second = true;
    return true;
}
bool ShaderTestOptions::Parse(int argumentCount, char** arguments)
{
    for (int index = 1; index < argumentCount; ++index) {
        const std::string_view argument{arguments[index]};
        if (SetFlag(argument)) continue;
        if (ParseFilter(argument, {arguments, static_cast<std::size_t>(argumentCount)}, index)) continue;
        std::cerr << "Usage: fo4hdr-shader-tests [--hardware] [--debug-layer] [--ae-runtime|--ng-runtime] "
                     "[--list|--filter <section>] "
                     "[--presentation-benchmark|--overlay-benchmark|--operator-lut-benchmark|--armor-encoded-only|--armor-encoded-benchmark]\n";
        return false;
    }
    armorEncodedMode = armorEncodedOnly || armorEncodedBenchmark;
    return true;
}
bool ShaderTestOptions::ParseFilter(std::string_view argument, std::span<char*> arguments, int& index)
{
    if (argument != "--filter") return false;
    if (static_cast<std::size_t>(index + 1) >= arguments.size()) return false;
    filtering = true;
    filter = arguments[++index];
    return true;
}
bool ShaderTestOptions::Benchmark() const
{
    return std::ranges::count(std::array{presentationBenchmark,
        overlayBenchmark, operatorLutBenchmark, armorEncodedBenchmark}, true) != 0;
}
bool ShaderTestOptions::ConflictingModes() const
{
    return std::ranges::count(std::array{armorEncodedOnly,
        armorEncodedBenchmark, operatorLutBenchmark,
        presentationBenchmark, overlayBenchmark}, true) > 1;
}
bool ShaderTestOptions::SelectionConflicts() const
{
    return (list || filtering) && (Benchmark() || armorEncodedMode);
}
bool ShaderTestOptions::ValidateModes() const
{
    const std::pair<bool, std::string_view> errors[]{
        {aeRuntime && ngRuntime, "AE and NG runtime selections are mutually exclusive.\n"},
        {ConflictingModes(), "Benchmark and armor-only modes are mutually exclusive.\n"},
        {Benchmark() && !hardware, "Benchmarks require --hardware.\n"},
        {debugLayer && Benchmark(), "Benchmarks require the debug layer disabled.\n"},
        {list && filtering, "--list and --filter are mutually exclusive.\n"},
        {SelectionConflicts(), "Selection and benchmark/armor-only modes are mutually exclusive.\n"}};
    const auto error = std::ranges::find(errors, true,
        &std::pair<bool, std::string_view>::first);
    if (error == std::end(errors)) return true;
    std::cerr << error->second;
    return false;
}

}  // namespace fo4hdr::tests::gpu
