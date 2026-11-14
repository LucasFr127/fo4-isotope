// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

namespace fo4hdr::tests {

void RunShaderTestOptionsTests();
void RunApproximateComparisonTests();
void RunBoundedWaitReleaseTests();
void RunHdrSettingsTests();
void RunDirectCallPatchTests();
void RunNativeUiResourcesTests();
void RunSnapshotTests();
void RunSnapshotLifecycleInvalidationTests();
void RunImageSpaceSemanticDataTests();
void RunSnapshotPublicationRaceTests();
void RunSnapshotConcurrentWritersTests();
void RunMenuEventQueueTests();
void RunMenuContextAndOverflowEvidenceTests();
void RunGameObservationPolicyTests();
void RunMenuDrainEvidenceTests();
void RunOwnedTaskTransferTests();
void RunMenuEventQueueOverwriteRaceTests();
void RunImageSpaceProvenanceTests();
void RunScopedD3D11ComputeConstantBufferTests();
void RunFrameTraceBufferTests();
void RunSharedUiContentTests();
void RunHookTests();
void RunArmorUiCompositionTests();
void RunHdrCoreTests();
void RunNativeLutObservationTests();
void RunNativeMenuStateTests();
void RunPerformanceTests();
void RunPipelineContractTests();
void RunRuntimeValidationTests();
void RunAgxTests();
void RunGt7Tests();
void RunOperatorLutTests();
void RunSdrOperatorLutTests();
void RunToneMappingTests();
void TestSettingsDefaultsAndMath();
void TestSettingsValidation();
void TestWeatherGradeConfiguration();
void TestWeatherGradeResponse();
void TestConfigurationValues();
void TestConfigurationRepairs();
void TestSettingsPersistenceRewrite();
void TestSettingsPersistenceSave();
void TestOperatorLutDeviceReplacementTransitions();
void TestOperatorLutWorkerAndPreparedCache();
void TestSdrOperatorLutRequests();
void TestSelectiveColorMathAndPersistence();
void TestToneMapperRecommendations();
void TestSettingsStatus();
void TestDeveloperExposureLockFramePolicy();
void TestWeatherResponseDrops();
void TestPipboyScreenObservation();

}  // namespace fo4hdr::tests
