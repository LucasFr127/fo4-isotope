// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include <F4SE/F4SE.h>

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ 0, 0, 0, 0 });
    v.PluginName("FO4Isotope");
    v.AuthorName("FO4Isotope contributors");
    // NG F4SE reads the 1.10.980 bits and AE F4SE the 1.11.137 bits; CommonLib
    // rejects any build it does not support at startup.
    v.UsesAddressLibraryNG(true);
    v.UsesAddressLibraryAE(true);
    v.IsLayoutDependentNG(true);
    v.IsLayoutDependentAE(true);
    v.CompatibleVersions({ F4SE::RUNTIME_LATEST });
    return v;
}();
