// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <cstdlib>

int wmain(int argc, wchar_t** argv)
{
    fo4hdr::tests::RunHdrCoreTests();
    fo4hdr::tests::RunPerformanceTests();
    fo4hdr::tests::RunToneMappingTests();
    fo4hdr::tests::RunOperatorLutTests();
    fo4hdr::tests::RunHdrOperatorWorkerTests();
    fo4hdr::tests::RunGt7Tests();
    fo4hdr::tests::RunPipelineContractTests();
    fo4hdr::tests::RunPipboyScreenObservationTests();
    fo4hdr::tests::RunHookTests();
    fo4hdr::tests::RunNativeImageSlotsTests();
    fo4hdr::tests::RunDeviceCreationHookTests();
    fo4hdr::tests::RunHdrSettingsMathTests();
    fo4hdr::tests::RunHdrPersistenceTests();
    fo4hdr::tests::RunShaderTestOptionsTests();
    fo4hdr::tests::RunDirectCallPatchTests();
    fo4hdr::tests::RunGameSnapshotStoreTests();
    fo4hdr::tests::RunMenuEventQueueTests();
    fo4hdr::tests::RunRuntimeValidationTests();
    fo4hdr::tests::RunRuntimeIdentityFilesTests();
    FO4HDR_CHECK(argc == 2);
    if (argc == 2)
    {
        fo4hdr::tests::RunPluginBootstrapSmokeTests(argv[1]);
    }
    return fo4hdr::test::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
