// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <string_view>

namespace fo4hdr::tests {

void RunHdrSettingsTests()
{
    TestPipboyScreenObservation();
    TestDeveloperExposureLockFramePolicy();
    TestHdrShortcutInput();
    TestHdrShortcutWindowProcedure();
    TestHotkeyFormatting();
    TestSettingsDefaultsAndMath();
    TestSettingsValidation();
    TestLutGradeProfiles();
    TestGradeProfilePacks();
    TestToneMapperPanelSmoke();
    TestVanillaGradePanelSmoke();
    TestOptionalConfigurationValues();
    TestOptionalConfigurationFailures();
    TestSettingsPersistenceRewrite();
    TestSettingsPersistenceSave();
    TestOperatorLutDeviceReplacementTransitions();
    TestOperatorLutWorkerAndPreparedCache();
    TestSelectiveColorMathAndPersistence();
}

}  // namespace fo4hdr::tests

namespace {

struct Section
{
    std::string_view name;
    void (*run)();
};

constexpr Section kSections[]{
    {"approximate-comparison", fo4hdr::tests::RunApproximateComparisonTests},
    {"bounded-wait-release", fo4hdr::tests::RunBoundedWaitReleaseTests},
    {"armor-ui-composition", fo4hdr::tests::RunArmorUiCompositionTests},
    {"armor-ui-observer-hook", fo4hdr::tests::RunDirectCallPatchTests},
    {"display-calibration", fo4hdr::tests::RunDisplayCalibrationTests},
    {"hooks", fo4hdr::tests::RunHookTests},
    {"pipeline-contract", fo4hdr::tests::RunPipelineContractTests},
    {"hdr-core", fo4hdr::tests::RunHdrCoreTests},
    {"tone-mapping", fo4hdr::tests::RunToneMappingTests},
    {"agx", fo4hdr::tests::RunAgxTests},
    {"gt7", fo4hdr::tests::RunGt7Tests},
    {"performance", fo4hdr::tests::RunPerformanceTests},
    {"hdr-settings", fo4hdr::tests::RunHdrSettingsTests},
    {"lut-profiles-panel", fo4hdr::tests::RunLutProfilesPanelTests},
    {"native-lut-observation", fo4hdr::tests::RunNativeLutObservationTests},
    {"image-space-provenance", fo4hdr::tests::RunImageSpaceProvenanceTests},
    {"scoped-d3d11-compute-constant-buffer",
        fo4hdr::tests::RunScopedD3D11ComputeConstantBufferTests},
    {"snapshot", fo4hdr::tests::RunSnapshotTests},
    {"snapshot-lifecycle-invalidation",
        fo4hdr::tests::RunSnapshotLifecycleInvalidationTests},
    {"image-space-semantic-data", fo4hdr::tests::RunImageSpaceSemanticDataTests},
    {"snapshot-publication-race",
        fo4hdr::tests::RunSnapshotPublicationRaceTests},
    {"snapshot-concurrent-writers",
        fo4hdr::tests::RunSnapshotConcurrentWritersTests},
    {"menu-event-queue", fo4hdr::tests::RunMenuEventQueueTests},
    {"menu-context-overflow-evidence",
        fo4hdr::tests::RunMenuContextAndOverflowEvidenceTests},
    {"menu-event-queue-overwrite-race",
        fo4hdr::tests::RunMenuEventQueueOverwriteRaceTests},
    {"game-observation-policy", fo4hdr::tests::RunGameObservationPolicyTests},
    {"menu-drain-evidence", fo4hdr::tests::RunMenuDrainEvidenceTests},
    {"owned-task-transfer", fo4hdr::tests::RunOwnedTaskTransferTests},
    {"runtime-validation", fo4hdr::tests::RunRuntimeValidationTests},
    {"runtime-identity-files", fo4hdr::tests::RunRuntimeIdentityFilesTests},
    {"shader-runner-options", fo4hdr::tests::RunShaderTestOptionsTests},
};

void RunSection(const Section& section)
{
    const auto failuresBefore = fo4hdr::test::failures;
    std::cout << "[ RUN ] " << section.name << std::endl;
    try
    {
        section.run();
    }
    catch (const std::exception& error)
    {
        ++fo4hdr::test::failures;
        std::cerr << "[ ERROR ] " << section.name << ": "
                  << error.what() << '\n';
    }
    catch (...)
    {
        ++fo4hdr::test::failures;
        std::cerr << "[ ERROR ] " << section.name
                  << ": unknown exception\n";
    }
    std::cout << "[ END ] " << section.name << ' '
              << (fo4hdr::test::failures == failuresBefore ? "passed" : "failed")
              << '\n';
}

struct CommandOptions
{
    bool list{};
    bool filtering{};
    std::string_view filter;
};

bool ParseCommandOptions(int argumentCount, char** arguments, CommandOptions& options)
{
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string_view argument{arguments[index]};
        if (argument == "--list")
        {
            options.list = true;
        }
        else if (argument == "--filter" && index + 1 < argumentCount)
        {
            options.filtering = true;
            options.filter = arguments[++index];
        }
        else
        {
            std::cerr << "Usage: fo4hdr-tests [--list|--filter <section>]\n";
            return false;
        }
    }
    if (options.list && options.filtering)
    {
        std::cerr << "--list and --filter are mutually exclusive.\n";
        return false;
    }
    return true;
}

int ListSections()
{
    for (const auto& section : kSections) std::cout << section.name << '\n';
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argumentCount, char** arguments)
{
    CommandOptions options;
    if (!ParseCommandOptions(argumentCount, arguments, options)) return 2;
    const auto [list, filtering, filter] = options;
    if (list) return ListSections();

    const auto selected = std::find_if(
        std::begin(kSections), std::end(kSections),
        [filter](const Section& section) { return section.name == filter; });
    if (filtering && selected == std::end(kSections))
    {
        std::cerr << "Unknown test section: " << filter << '\n';
        return 2;
    }

    std::cout << "[ SUITE START ] native-cpu" << std::endl;
    if (!filtering)
    {
        std::for_each(std::begin(kSections), std::end(kSections), RunSection);
    }
    else
    {
        RunSection(*selected);
    }

    if (fo4hdr::test::failures != 0)
    {
        std::cout << "[ SUITE END ] native-cpu failed\n";
        std::cerr << fo4hdr::test::failures
                  << " test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "[ SUITE END ] native-cpu passed\n";
    if (filtering)
        std::cout << "Selected Fallout4HDR test section passed.\n";
    else
        std::cout << "All Fallout4HDR tests passed.\n";
    return EXIT_SUCCESS;
}
