// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"
#include "TestHarness.hpp"
#include "DirectCallPatch.hpp"
#include <Windows.h>
#include <cstring>

namespace fo4hdr::tests {
namespace {
namespace hook = game::direct_call_patch;
template<std::size_t N>
struct Backend
{
    std::array<hook::CallBytes, N> memory{};
    std::size_t writes{}, failWrite{}, foreignWrite{};
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
void NativeScreen(void* object) noexcept { ++g_calls; g_object = object; }
void ForwardScreen(void* object) noexcept { NativeScreen(object); }

template<std::size_t N>
Backend<N> OriginalBackend(const std::array<hook::Site, N>& sites)
{
    Backend<N> backend{};
    for (std::size_t index = 0; index < N; ++index)
        backend.memory[index] = sites[index].original;
    return backend;
}
template<std::size_t N>
void CheckCallPatchOwnership(const std::array<hook::Site, N>& sites)
{
    auto backend = OriginalBackend(sites);
    FO4HDR_CHECK(hook::InstallPair(sites, backend));
    FO4HDR_CHECK(backend.writes == N);
    FO4HDR_CHECK(!hook::InstallPair(sites, backend)); // no second mutation
    FO4HDR_CHECK(backend.writes == N);
    for (std::size_t index = 0; index < N; ++index) {
        backend = OriginalBackend(sites);
        backend.memory[index][0] = 0xCC;
        FO4HDR_CHECK(!hook::InstallPair(sites, backend));
        FO4HDR_CHECK(backend.writes == 0);
    }
    for (std::size_t fail = 1; fail <= N; ++fail) {
        backend = OriginalBackend(sites);
        backend.failWrite = fail; // write succeeded, protection/cache failed
        FO4HDR_CHECK(!hook::InstallPair(sites, backend));
        for (std::size_t index = 0; index < N; ++index)
            FO4HDR_CHECK(backend.memory[index] == sites[index].original);
    }
    backend = OriginalBackend(sites);
    backend.failWrite = N; backend.foreignWrite = N;
    FO4HDR_CHECK(!hook::InstallPair(sites, backend));
    for (std::size_t index = 0; index + 1 < N; ++index)
        FO4HDR_CHECK(backend.memory[index] == sites[index].original);
    FO4HDR_CHECK(backend.memory.back()[0] == 0xCC);
    backend = OriginalBackend(sites);
    backend.failWrite = N; backend.failRollback = true;
    FO4HDR_CHECK(!hook::InstallPair(sites, backend));
    FO4HDR_CHECK(backend.memory[0] == sites[0].replacement);
}
void WriteCallFixture(std::uint8_t* memory, std::size_t offset, std::uintptr_t relay)
{
    hook::CallBytes bytes{};
    const std::uint8_t prefix[]{0x48, 0x83, 0xEC, 0x28};
    const std::uint8_t suffix[]{0x48, 0x83, 0xC4, 0x28, 0xC3};
    std::memcpy(memory + offset, prefix, 4);
    FO4HDR_CHECK(hook::EncodeCall(reinterpret_cast<std::uintptr_t>(memory + offset + 4), relay, bytes));
    std::memcpy(memory + offset + 4, bytes.data(), bytes.size());
    std::memcpy(memory + offset + 9, suffix, 5);
}
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
    const std::array<hook::Site, 2> armorSites{{
        {1, {0xE8, 1, 0, 0, 0}, {0xE8, 5, 0, 0, 0}, 0},
        {2, {0xE8, 2, 0, 0, 0}, {0xE8, 6, 0, 0, 0}, 0}}};
    const std::array<hook::Site, 4> screenSites{{
        armorSites[0], armorSites[1],
        {3, {0xE8, 3, 0, 0, 0}, {0xE8, 7, 0, 0, 0}, 0},
        {4, {0xE8, 4, 0, 0, 0}, {0xE8, 8, 0, 0, 0}, 0}}};
    CheckCallPatchOwnership(armorSites);
    CheckCallPatchOwnership(screenSites);
}

void RunCallForwardingTests()
{
    // Exercise Windows x64 opaque RCX/EDX and RCX-only forwarding through direct
    // CALL offsets and the same FF25 relay shape used by the adapter. No game ABI
    // facts are inferred from this disposable executable allocation.
    auto* memory = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FO4HDR_CHECK(memory != nullptr);
    if (!memory) return;
    const auto relay = reinterpret_cast<std::uintptr_t>(memory + 128);
    memory[128] = 0xFF; memory[129] = 0x25;
    const auto destination = reinterpret_cast<std::uintptr_t>(&Forward);
    std::memcpy(memory + 134, &destination, 8);
    for (std::size_t offset : {std::size_t{0}, std::size_t{64}})
        WriteCallFixture(memory, offset, relay);
    const auto screenRelay = reinterpret_cast<std::uintptr_t>(memory + 384);
    memory[384] = 0xFF; memory[385] = 0x25;
    const auto screenDestination = reinterpret_cast<std::uintptr_t>(&ForwardScreen);
    std::memcpy(memory + 390, &screenDestination, 8);
    for (std::size_t offset : {std::size_t{192}, std::size_t{224}, std::size_t{256}, std::size_t{288}})
        WriteCallFixture(memory, offset, screenRelay);
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
        using ScreenCall = void (*)(void*);
        std::uint32_t expectedCalls = 2;
        for (std::size_t offset : {std::size_t{192}, std::size_t{224}, std::size_t{256}, std::size_t{288}}) {
            reinterpret_cast<ScreenCall>(memory + offset)(memory + offset);
            ++expectedCalls;
            FO4HDR_CHECK(g_calls == expectedCalls);
            FO4HDR_CHECK(g_object == memory + offset);
        }
    }
    FO4HDR_CHECK(::VirtualFree(memory, 0, MEM_RELEASE) != FALSE);
}
void RunDirectCallPatchTests()
{
    RunCallPatchOwnershipTests();
    RunCallForwardingTests();
}
}
