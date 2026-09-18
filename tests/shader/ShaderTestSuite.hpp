// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

namespace fo4hdr::tests::gpu {
void VerifyNativeReferencePreparation(const std::filesystem::path& fixtureDirectory);
void VerifyShippingPixelShaders(const std::filesystem::path& fixtureDirectory);
void VerifyPresentationPass(const std::filesystem::path& fixtureDirectory);
void VerifyDisplayCalibration(const std::filesystem::path& fixtureDirectory);
void VerifyArmorEncodedComposition(const std::filesystem::path& directory);
void VerifyImageControls(const std::filesystem::path& directory);
void VerifyCreativeGradeLut(const std::filesystem::path& fixtureDirectory);
void VerifyHdrGradeTransfer(const std::filesystem::path& shaderPath);
void BenchmarkOperatorLut(const std::filesystem::path& bytecodePath);
void VerifyOperatorLut(const std::filesystem::path& fixtureDirectory);
void VerifyDeveloperExposure(const std::filesystem::path& fixtureDirectory);
}

namespace fo4hdr::tests {
void RunSettingsPanelTests();
}

