// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <span>
#include <string_view>

namespace fo4hdr::tests::gpu {

struct ShaderTestOptions
{
    bool hardware{};
    bool debugLayer{};
    bool list{};
    bool filtering{};
    bool armorEncodedOnly{};
    bool armorEncodedBenchmark{};
    bool operatorLutBenchmark{};
    bool presentationBenchmark{};
    bool overlayBenchmark{};
    std::string_view filter;
    bool armorEncodedMode{};

    bool Parse(int argumentCount, char** arguments);
    bool ValidateModes() const;

private:
    bool SetFlag(std::string_view argument);
    bool ParseFilter(std::string_view argument, std::span<char*> arguments, int& index);
    bool Benchmark() const;
    bool ConflictingModes() const;
    bool SelectionConflicts() const;
};

}  // namespace fo4hdr::tests::gpu
