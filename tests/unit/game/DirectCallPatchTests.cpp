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
    CheckCallPatchOwnership(std::array<hook::Site, 1>{armorSites[0]});
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
void RunSiteMismatchTests()
{
    (void)hook::TakeSiteMismatch();
    const std::array<std::uint8_t, 4> memory{0x48, 0x89, 0x5C, 0x24};
    const auto address = reinterpret_cast<std::uintptr_t>(memory.data());
    const auto read = [](const void* source, void* destination, std::size_t size) noexcept {
        std::memcpy(destination, source, size);
        return true;
    };
    FO4HDR_CHECK(hook::MatchesNativeCode(address, memory, read));
    FO4HDR_CHECK(!hook::TakeSiteMismatch().recorded);
    // The first rejection is retained with its observed bytes; later ones are ignored.
    const std::array<std::uint8_t, 4> expected{0x40, 0x53, 0x48, 0x83};
    FO4HDR_CHECK(!hook::MatchesNativeCode(address, expected, read));
    FO4HDR_CHECK(!hook::MatchesNativeCode(address + 1, expected, read));
    auto mismatch = hook::TakeSiteMismatch();
    FO4HDR_CHECK(mismatch.recorded && mismatch.address == address && mismatch.id == 0);
    FO4HDR_CHECK(mismatch.observedSize == memory.size() &&
        std::equal(memory.begin(), memory.end(), mismatch.observed.begin()));
    FO4HDR_CHECK(!hook::TakeSiteMismatch().recorded);

    // IDs resolve to whatever RVA the running build maps; only absence rejects.
    struct Mapping { std::uint64_t id; std::uintptr_t offset; };
    const std::array<Mapping, 3> mappings{{{7, 0x70}, {9, 0x90}, {13, 0}}};
    FO4HDR_CHECK(hook::ResolveMapping(mappings, 0x1000, 7) == 0x70);
    FO4HDR_CHECK(hook::ResolveMapping(mappings, 0x1000, 9) == 0x90);
    FO4HDR_CHECK(!hook::TakeSiteMismatch().recorded);
    FO4HDR_CHECK(hook::ResolveMapping(mappings, 0x1000, 11) == 0);
    mismatch = hook::TakeSiteMismatch();
    FO4HDR_CHECK(mismatch.address == 0x1000 && mismatch.id == 11 && mismatch.mappedRva == 0 &&
        mismatch.observedSize == 0);
    FO4HDR_CHECK(hook::ResolveMapping(mappings, 0x1000, 13) == 0);
    FO4HDR_CHECK(hook::TakeSiteMismatch().id == 13);
}
void RunCallQualificationTests()
{
    (void)hook::TakeSiteMismatch();
    const auto read = [](const void* source, void* destination, std::size_t size) noexcept {
        std::memcpy(destination, source, size);
        return true;
    };
    std::array<std::uint8_t, 5> code{};
    const auto site = reinterpret_cast<std::uintptr_t>(code.data());
    const auto target = site + 5 + 0x1234;
    FO4HDR_CHECK(hook::EncodeCall(site, target, code));
    hook::CallBytes observed{};
    // The live call to the resolved target qualifies, whatever its displacement.
    FO4HDR_CHECK(hook::ReadCallTo(site, target, observed, read) && observed == code);
    FO4HDR_CHECK(!hook::TakeSiteMismatch().recorded);
    // A call elsewhere, a non-call, an unreadable site or no target rejects.
    FO4HDR_CHECK(!hook::ReadCallTo(site, target + 1, observed, read));
    auto mismatch = hook::TakeSiteMismatch();
    FO4HDR_CHECK(mismatch.address == site && mismatch.observedSize == code.size() &&
        std::equal(code.begin(), code.end(), mismatch.observed.begin()));
    code[0] = 0xE9;
    FO4HDR_CHECK(!hook::ReadCallTo(site, target, observed, read));
    (void)hook::TakeSiteMismatch();
    code[0] = 0xE8;
    FO4HDR_CHECK(!hook::ReadCallTo(site, 0, observed, read));
    (void)hook::TakeSiteMismatch();
    const auto unreadable = [](const void*, void*, std::size_t) noexcept { return false; };
    FO4HDR_CHECK(!hook::ReadCallTo(site, target, observed, unreadable));
    FO4HDR_CHECK(hook::TakeSiteMismatch().observedSize == 0);
}
void RunDirectCallPatchTests()
{
    RunCallPatchOwnershipTests();
    RunCallForwardingTests();
    RunSiteMismatchTests();
    RunCallQualificationTests();
}
}
