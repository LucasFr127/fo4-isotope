// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include <F4SE/F4SE.h>

F4SE_PLUGIN_VERSION = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ 0, 0, 0, 0 });
    v.PluginName("FO4Isotope");
    v.AuthorName("FO4Isotope contributors");
    v.UsesAddressLibraryNG(true);
    v.UsesSigScanning(false);
    v.IsLayoutDependentNG(true);
    v.HasNoStructUse(false);
    v.CompatibleVersions({ F4SE::RUNTIME_1_10_984 });
    v.MinimumRequiredXSEVersion({ 0, 7, 2, 0 });
    return v;
}();
