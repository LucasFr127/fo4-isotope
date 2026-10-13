// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "d3d11/GpuHarness.hpp"
#include "reference/ShaderReference.hpp"

int VerifyOperatorLutSampling(ID3D11Device& device, ID3D11DeviceContext& context, bool benchmark);

namespace fo4hdr::tests::gpu {

bool VerifyArmorEncodedComposition(Device& device, bool benchmark=false);

void VerifyCalibrationPatterns(Device& device);

void VerifyContextQuerySafety();

void VerifyFrameTrace(Device& device);

void VerifyDirectPresentation(Device& device);
void VerifyPublicPresentationOutput(Device& device);

void VerifyPresentationResize(Device& device);

void VerifyPresentationSwapChain(Device& device);

void VerifySharedUiComposition(Device& device);

void BenchmarkDirectPresentation(Device& device, bool compareHiddenOverlay = false);

void VerifyGt7OperatorAndUi(Device& device);

void VerifyGt7OfficialReference(Device& device);

void VerifyLayerTransferReference(Device& device);

void VerifyNativeReferenceState(Device& device);

void VerifyNativeVanillaGradeRemoval(Device& device);

void VerifyScopedPixelBindingRestoration(Device& device);

void VerifyPresentationProxy(Device& device);

void VerifyOutputDitherExperiment(Device& device);

void VerifyPresentationState(Device& device);
void VerifyPresentationContext(Device& device);
void VerifyNativeSceneInitialization(Device& device);
void VerifySharedUiContext(Device& device);
void VerifySharedUiFallback(Device& device);
void VerifySharedSdrFrame(Device& device);

void VerifyUiRelativeTemporalTransport(TestedToneMapper toneMapper);

void VerifyShippingPixelShaders(Device& device);

void VerifyLutGradeProfiles(Device& device);

}  // namespace fo4hdr::tests::gpu
