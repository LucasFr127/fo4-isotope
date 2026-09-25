// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"
#include "TestHarness.hpp"
#include "DirectCallPatch.hpp"
#include <Windows.h>
#include <cstring>

namespace fo4hdr::tests {
namespace {
namespace hook = game::direct_call_patch;
struct Backend
{
    std::array<hook::CallBytes, 2> memory{};
    int writes{}, failWrite{}, foreignWrite{};
    bool failRollback{};
    bool Matches(std::uintptr_t address, const hook::CallBytes& value) noexcept
    { return memory[address - 1] == value; }
    bool Replace(const hook::Site& site, const hook::CallBytes& before,
        const hook::CallBytes& after) noexcept
    {
        ++writes;
        if (!Matches(site.address, before)) return false;
        if (failRollback && after == site.original) return false;
        memory[site.address - 1] = after;
        if (writes == foreignWrite) memory[site.address - 1][0] = 0xCC;
        return writes != failWrite;
    }
};
std::uint32_t g_calls{}, g_argument{};
void* g_object{};
void Native(void* object, std::uint32_t argument) noexcept
{ ++g_calls; g_object = object; g_argument = argument; }
void Forward(void* object, std::uint32_t argument) noexcept { Native(object, argument); }
}
void RunCallPatchOwnershipTests()
{
    hook::CallBytes bytes{};
    FO4HDR_CHECK(hook::EncodeCall(0x1000, 0x1800, bytes));
    FO4HDR_CHECK(hook::CallTarget(0x1000, bytes) == 0x1800);
    FO4HDR_CHECK(hook::EncodeCall(0x2000, 0x1000, bytes));
    FO4HDR_CHECK(hook::CallTarget(0x2000, bytes) == 0x1000);
    FO4HDR_CHECK(!hook::EncodeCall(0x1000, 0x90001005, bytes));
    FO4HDR_CHECK(!hook::EncodeCall(UINTPTR_MAX, 1, bytes));
    bytes[0] = 0xE9;
    FO4HDR_CHECK(hook::CallTarget(0x2000, bytes) == 0);
    const std::array<hook::Site, 2> sites{{
        {1, {0xE8, 1, 0, 0, 0}, {0xE8, 3, 0, 0, 0}, 0},
        {2, {0xE8, 2, 0, 0, 0}, {0xE8, 4, 0, 0, 0}, 0}}};
    Backend backend{{sites[0].original, sites[1].original}};
    FO4HDR_CHECK(hook::InstallPair(sites, backend));
    FO4HDR_CHECK(backend.writes == 2);
    FO4HDR_CHECK(!hook::InstallPair(sites, backend)); // no second mutation
    FO4HDR_CHECK(backend.writes == 2);
    backend = {{sites[0].original, sites[1].original}};
    backend.memory[1][0] = 0xCC;
    FO4HDR_CHECK(!hook::InstallPair(sites, backend));
    FO4HDR_CHECK(backend.writes == 0);
    for (int fail = 1; fail <= 2; ++fail) {
        backend = {{sites[0].original, sites[1].original}};
        backend.failWrite = fail; // write succeeded, protection/cache failed
        FO4HDR_CHECK(!hook::InstallPair(sites, backend));
        FO4HDR_CHECK(backend.memory[0] == sites[0].original);
        FO4HDR_CHECK(backend.memory[1] == sites[1].original);
    }
    backend = {{sites[0].original, sites[1].original}};
    backend.failWrite = 2; backend.foreignWrite = 2;
    FO4HDR_CHECK(!hook::InstallPair(sites, backend));
    FO4HDR_CHECK(backend.memory[0] == sites[0].original);
    FO4HDR_CHECK(backend.memory[1][0] == 0xCC);
    backend = {{sites[0].original, sites[1].original}};
    backend.failWrite = 2; backend.failRollback = true;
    FO4HDR_CHECK(!hook::InstallPair(sites, backend));
    FO4HDR_CHECK(backend.memory[0] == sites[0].replacement);

}

void RunCallForwardingTests()
{
    hook::CallBytes bytes{};
    // Exercise Windows x64 opaque RCX/EDX forwarding through both direct CALL
    // offsets and the same FF25 relay shape used by the adapter. No game ABI
    // facts are inferred from this disposable executable allocation.
    auto* memory = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FO4HDR_CHECK(memory != nullptr);
    if (!memory) return;
    const auto relay = reinterpret_cast<std::uintptr_t>(memory + 128);
    memory[128] = 0xFF; memory[129] = 0x25;
    const auto destination = reinterpret_cast<std::uintptr_t>(&Forward);
    std::memcpy(memory + 134, &destination, 8);
    for (std::size_t offset : {std::size_t{0}, std::size_t{64}}) {
        const std::uint8_t prefix[]{0x48, 0x83, 0xEC, 0x28};
        const std::uint8_t suffix[]{0x48, 0x83, 0xC4, 0x28, 0xC3};
        std::memcpy(memory + offset, prefix, 4);
        FO4HDR_CHECK(hook::EncodeCall(reinterpret_cast<std::uintptr_t>(memory + offset + 4), relay, bytes));
        std::memcpy(memory + offset + 4, bytes.data(), bytes.size());
        std::memcpy(memory + offset + 9, suffix, 5);
    }
    DWORD prior{};
    const bool executable = ::VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &prior) &&
        ::FlushInstructionCache(::GetCurrentProcess(), memory, 4096);
    FO4HDR_CHECK(executable);
    if (executable) {
        using Call = void (*)(void*, std::uint32_t);
        g_calls = 0;
        reinterpret_cast<Call>(memory)(memory, 0xF1234567u);
        FO4HDR_CHECK(g_calls == 1);
        FO4HDR_CHECK(g_object == memory);
        FO4HDR_CHECK(g_argument == 0xF1234567u);
        reinterpret_cast<Call>(memory + 64)(memory + 64, 0x87654321u);
        FO4HDR_CHECK(g_calls == 2);
        FO4HDR_CHECK(g_object == memory + 64);
        FO4HDR_CHECK(g_argument == 0x87654321u);
    }
    FO4HDR_CHECK(::VirtualFree(memory, 0, MEM_RELEASE) != FALSE);
}
void RunDirectCallPatchTests()
{
    RunCallPatchOwnershipTests();
    RunCallForwardingTests();
}
}
