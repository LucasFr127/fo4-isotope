// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct IDXGISwapChain;

namespace fo4hdr::tests {

void RunHdrCoreTests();
void RunPerformanceTests();
void RunToneMappingTests();
void RunHdrSelectiveColorTests();
void RunAgxTests();
void RunOperatorLutTests();
void RunHdrOperatorWorkerTests();
void RunGt7Tests();
void RunPipelineContractTests();
void RunPipboyScreenObservationTests();
void RunHookTests();
void RunNativeImageSlotsTests();
void RunDeviceCreationHookTests();
void RunSwapChainObservationTests(void (*create)(IDXGISwapChain*));
void RunHdrSettingsMathTests();
void RunHdrConfigurationTests();
void TestLutGradeProfiles();
void RunHdrPersistenceTests();
void RunShaderTestOptionsTests();
void RunDirectCallPatchTests();
void RunGameSnapshotStoreTests();
void RunImageSpaceProvenanceTests();
void RunLutProfilesPanelTests();
void RunMenuEventQueueTests();
void RunRuntimeValidationTests();
void RunRuntimeIdentityFilesTests();
void RunPluginBootstrapSmokeTests(const wchar_t* pluginPath);

}  // namespace fo4hdr::tests
