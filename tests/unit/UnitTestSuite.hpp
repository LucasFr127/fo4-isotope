// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

namespace fo4hdr::tests {

void RunFrameTraceBufferTests();
void RunSharedUiContentTests();
void RunShaderTestOptionsTests();
void RunApproximateComparisonTests();
void RunBoundedWaitReleaseTests();
void RunHdrSettingsTests();
void TestHdrShortcutInput();
void TestHdrShortcutWindowProcedure();
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
void TestToneMapperPanelSmoke();
void TestVanillaGradePanelSmoke();
void TestHotkeyFormatting();
void TestDeveloperExposureLockFramePolicy();
void TestPipboyScreenObservation();

}  // namespace fo4hdr::tests
