// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include <F4SE/F4SE.h>

F4SE_PLUGIN_VERSION = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ 0, 0, 0, 0 });
    v.PluginName("Fallout4HDR");
    v.AuthorName("Fallout4HDR contributors");
    v.UsesAddressLibrary(true);
    v.UsesSigScanning(false);
    v.IsLayoutDependent(true);
    v.HasNoStructUse(false);
    v.CompatibleVersions({ F4SE::RUNTIME_1_10_163 });
    v.MinimumRequiredXSEVersion({ 0, 6, 23, 0 });
    return v;
}();
