// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fo4hdr::tests {

void RunHdrCoreTests();
void RunRuntimeValidationTests();
void RunRuntimeIdentityFilesTests();
void RunPluginBootstrapSmokeTests(const wchar_t* pluginPath);

}  // namespace fo4hdr::tests
