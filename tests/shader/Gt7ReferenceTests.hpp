// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <filesystem>

namespace fo4hdr::tests::gpu {

[[nodiscard]] double VerifyGt7Reference(const std::filesystem::path& shaderPath);

}  // namespace fo4hdr::tests::gpu
