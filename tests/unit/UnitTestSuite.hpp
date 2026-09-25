// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

namespace fo4hdr::tests {

void RunShaderTestOptionsTests();
void RunApproximateComparisonTests();
void RunBoundedWaitReleaseTests();
void RunHdrSettingsTests();
void RunDirectCallPatchTests();
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
void RunHookTests();
void RunArmorUiCompositionTests();
void RunHdrCoreTests();
void RunNativeLutObservationTests();
void RunPerformanceTests();
void RunPipelineContractTests();
void RunRuntimeIdentityFilesTests();
void RunRuntimeValidationTests();
void RunAgxTests();
void RunGt7Tests();
void RunOperatorLutTests();
void RunToneMappingTests();
void RunDisplayCalibrationTests();
void RunLutProfilesPanelTests();
void RunSettingsPanelTests();
void TestSettingsDefaultsAndMath();
void TestSettingsValidation();
void TestLutGradeProfiles();
void TestGradeProfilePacks();
void TestOptionalConfigurationValues();
void TestOptionalConfigurationFailures();
void TestSettingsPersistenceRewrite();
void TestSettingsPersistenceSave();
void TestOperatorLutDeviceReplacementTransitions();
void TestOperatorLutWorkerAndPreparedCache();
void TestSelectiveColorMathAndPersistence();
void TestDeveloperExposureLockFramePolicy();
void TestPipboyScreenObservation();

}  // namespace fo4hdr::tests
