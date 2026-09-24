// SPDX-License-Identifier: GPL-3.0-or-later

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
    TestSettingsDefaultsAndMath();
    TestSettingsValidation();
    TestLutGradeProfiles();
    TestGradeProfilePacks();
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

}  // namespace

int main()
{
    std::cout << "[ SUITE START ] native-cpu" << std::endl;
    std::for_each(std::begin(kSections), std::end(kSections), RunSection);
    if (fo4hdr::test::failures != 0)
    {
        std::cout << "[ SUITE END ] native-cpu failed\n";
        std::cerr << fo4hdr::test::failures << " test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "[ SUITE END ] native-cpu passed\n";
    return EXIT_SUCCESS;
}
