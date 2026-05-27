// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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

struct PluginInfo
{
    std::uint32_t infoVersion;
    const char* name;
    std::uint32_t version;
};

struct PluginVersionData
{
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char pluginName[256];
    char author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t xseMinimum;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
    std::uint8_t reserved[512];
};

// F4SE exports a fixed data layout, independent of the C++ wrapper types.
static_assert(offsetof(PluginVersionData, dataVersion) == 0x000);
static_assert(offsetof(PluginVersionData, pluginVersion) == 0x004);
static_assert(offsetof(PluginVersionData, pluginName) == 0x008);
static_assert(offsetof(PluginVersionData, author) == 0x108);
static_assert(offsetof(PluginVersionData, addressIndependence) == 0x208);
static_assert(offsetof(PluginVersionData, structureIndependence) == 0x20C);
static_assert(offsetof(PluginVersionData, compatibleVersions) == 0x210);
static_assert(offsetof(PluginVersionData, xseMinimum) == 0x250);
static_assert(offsetof(PluginVersionData, reservedNonBreaking) == 0x254);
static_assert(offsetof(PluginVersionData, reservedBreaking) == 0x258);
static_assert(offsetof(PluginVersionData, reserved) == 0x25C);
static_assert(sizeof(PluginVersionData) == 0x45C);
static_assert(sizeof(F4seInterface) == 0x38);
static_assert(offsetof(F4seInterface, queryInterface) == 0x10);
static_assert(sizeof(PluginInfo) == 0x18);
static_assert(offsetof(PluginInfo, name) == 0x08);
static_assert(offsetof(PluginInfo, version) == 0x10);
static_assert(FO4HDR_TEST_EXECUTABLE_BUILD == 0 || FO4HDR_TEST_EXECUTABLE_BUILD == 1);

using Query = bool (__cdecl*)(const F4seInterface*, PluginInfo*);
using Load = bool (__cdecl*)(const F4seInterface*);

constexpr F4seInterface supportedF4se{0x00060170, 0x010A0A30};
constexpr bool executableMatches = FO4HDR_TEST_EXECUTABLE_BUILD == 0;

void CheckVersionData(const PluginVersionData& data)
{
    constexpr char expectedName[256] = "Fallout4HDR";
    FO4HDR_CHECK(data.dataVersion == 1);
    FO4HDR_CHECK(data.pluginVersion == 0x00010000);
    FO4HDR_CHECK(std::ranges::equal(data.pluginName, expectedName));
    FO4HDR_CHECK(std::ranges::all_of(data.author, [](char value) { return value == 0; }));
    FO4HDR_CHECK(data.addressIndependence == 0);
    FO4HDR_CHECK(data.structureIndependence == 1);
    FO4HDR_CHECK(data.compatibleVersions[0] == supportedF4se.runtimeVersion);
    FO4HDR_CHECK(std::ranges::all_of(std::span{data.compatibleVersions}.subspan(1),
        [](std::uint32_t value) { return value == 0; }));
    FO4HDR_CHECK(data.xseMinimum == supportedF4se.f4seVersion);
    FO4HDR_CHECK(data.reservedNonBreaking == 0);
    FO4HDR_CHECK(data.reservedBreaking == 0);
    FO4HDR_CHECK(std::ranges::all_of(data.reserved,
        [](std::uint8_t value) { return value == 0; }));
}

void CheckQueryInfo(Query query)
{
    PluginInfo info{};
    FO4HDR_CHECK(query(&supportedF4se, &info) == executableMatches);
    FO4HDR_CHECK(info.infoVersion == 1);
    FO4HDR_CHECK(info.version == 0x00010000);
    FO4HDR_CHECK(info.name != nullptr);
    if (info.name != nullptr)
    {
        FO4HDR_CHECK(std::string_view{info.name} == "Fallout4HDR");
    }
}

void CheckRejectedIdentity(Query query, Load load, const F4seInterface& f4se)
{
    PluginInfo info{};
    FO4HDR_CHECK(!query(&f4se, &info));
    FO4HDR_CHECK(!load(&f4se));
}

void CheckRejectedInputs(Query query, Load load)
{
    PluginInfo info{};
    FO4HDR_CHECK(!query(nullptr, &info));
    FO4HDR_CHECK(!query(&supportedF4se, nullptr));
    FO4HDR_CHECK(!query(nullptr, nullptr));
    FO4HDR_CHECK(!load(nullptr));
    CheckRejectedIdentity(query, load, {});

    auto f4se = supportedF4se;
    f4se.isEditor = 1;
    CheckRejectedIdentity(query, load, f4se);
    f4se = supportedF4se;
    f4se.f4seVersion = 0;
    CheckRejectedIdentity(query, load, f4se);
    f4se = supportedF4se;
    f4se.runtimeVersion = 0;
    CheckRejectedIdentity(query, load, f4se);

    constexpr std::array componentShifts{24U, 16U, 4U, 0U};
    for (const auto shift : componentShifts)
    {
        f4se = supportedF4se;
        f4se.runtimeVersion += 1U << shift;
        CheckRejectedIdentity(query, load, f4se);
        f4se = supportedF4se;
        f4se.f4seVersion += 1U << shift;
        CheckRejectedIdentity(query, load, f4se);
    }
}

void CheckStartup(Query query, Load load)
{
    CheckRejectedInputs(query, load);
    CheckQueryInfo(query);
    FO4HDR_CHECK(load(&supportedF4se) == executableMatches);
    FO4HDR_CHECK(load(&supportedF4se) == executableMatches);
    CheckRejectedInputs(query, load);
    CheckQueryInfo(query);
}

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

    const auto query = reinterpret_cast<Query>(GetProcAddress(module, "F4SEPlugin_Query"));
    const auto load = reinterpret_cast<Load>(GetProcAddress(module, "F4SEPlugin_Load"));
    const auto version = reinterpret_cast<const PluginVersionData*>(
        GetProcAddress(module, "F4SEPlugin_Version"));
    FO4HDR_CHECK(query != nullptr);
    FO4HDR_CHECK(load != nullptr);
    FO4HDR_CHECK(version != nullptr);
    if (version != nullptr)
    {
        CheckVersionData(*version);
    }
    if (query != nullptr && load != nullptr)
    {
        CheckStartup(query, load);
    }
    FO4HDR_CHECK(FreeLibrary(module));
}

}  // namespace fo4hdr::tests
