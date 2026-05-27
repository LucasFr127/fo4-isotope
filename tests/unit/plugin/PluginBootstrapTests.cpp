// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <Windows.h>

#include <cstdint>

namespace {

struct F4seInterface
{
    std::uint32_t f4seVersion;
    std::uint32_t runtimeVersion;
    std::uint32_t editorVersion;
    std::uint32_t isEditor;
    void* (__cdecl* queryInterface)(std::uint32_t);
    std::uint32_t (__cdecl* getPluginHandle)();
    std::uint32_t (__cdecl* getReleaseIndex)();
    const void* (__cdecl* getPluginInfo)(const char*);
    const char* (__cdecl* getSaveFolderName)();
};

}  // namespace

namespace fo4hdr::tests {

void RunPluginBootstrapSmokeTests(const wchar_t* pluginPath)
{
    const auto module = LoadLibraryW(pluginPath);
    FO4HDR_CHECK(module != nullptr);
    if (module == nullptr)
    {
        return;
    }

    using Query = bool (__cdecl*)(const F4seInterface*, void*);
    using Load = bool (__cdecl*)(const F4seInterface*);
    const auto query = reinterpret_cast<Query>(GetProcAddress(module, "F4SEPlugin_Query"));
    const auto load = reinterpret_cast<Load>(GetProcAddress(module, "F4SEPlugin_Load"));
    FO4HDR_CHECK(query != nullptr);
    FO4HDR_CHECK(load != nullptr);
    FO4HDR_CHECK(GetProcAddress(module, "F4SEPlugin_Version") != nullptr);
    if (query != nullptr)
    {
        FO4HDR_CHECK(!query(nullptr, nullptr));
    }
    if (load != nullptr)
    {
        FO4HDR_CHECK(!load(nullptr));
        const F4seInterface f4se{0x00060170, 0x010A0A30};
        FO4HDR_CHECK(load(&f4se));
        FO4HDR_CHECK(load(&f4se));
    }
    FO4HDR_CHECK(FreeLibrary(module));
}

}  // namespace fo4hdr::tests
