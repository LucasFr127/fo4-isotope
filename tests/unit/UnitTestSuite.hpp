// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct IDXGISwapChain;

namespace fo4hdr::tests {

void RunHdrCoreTests();
void RunToneMappingTests();
void RunOperatorLutTests();
void RunHdrOperatorWorkerTests();
void RunGt7Tests();
void RunPipelineContractTests();
void RunPresentationCharacterizationTests();
void RunHookTests();
void RunDeviceCreationHookTests();
void RunSwapChainObservationTests(void (*create)(IDXGISwapChain*));
void RunHdrSettingsMathTests();
void RunHdrPersistenceTests();
void RunGameSnapshotStoreTests();
void RunMenuEventQueueTests();
void RunRuntimeValidationTests();
void RunRuntimeIdentityFilesTests();
void RunPluginBootstrapSmokeTests(const wchar_t* pluginPath);

}  // namespace fo4hdr::tests
