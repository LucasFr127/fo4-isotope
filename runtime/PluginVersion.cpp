// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include <F4SE/F4SE.h>

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ 0, 0, 0, 0 });
    v.PluginName("FO4Isotope");
    v.AuthorName("FO4Isotope contributors");
    v.UsesAddressLibraryAE(true);
    // Exact-version layout dependence makes AE enforce CompatibleVersions.
    v.CompatibleVersions({ F4SE::RUNTIME_1_10_163, F4SE::RUNTIME_1_11_240 });
    v.MinimumRequiredXSEVersion({ 0, 6, 23, 0 });
    return v;
}();
