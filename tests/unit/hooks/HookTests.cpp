// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"

#include "FO4Isotope/hooks/HookOwnership.hpp"
#include "FO4Isotope/hooks/PeIat.hpp"
#include "TestHarness.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace fo4hdr::tests {
namespace {

template <class T>
void Write(std::vector<std::byte>& image, std::size_t offset, T value)
{
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

std::vector<std::byte> MakePeImage()
{
    std::vector<std::byte> image(0x1000);
    Write<std::uint16_t>(image, 0, 0x5A4D);
    Write<std::uint32_t>(image, 0x3C, 0x100);
    Write<std::uint32_t>(image, 0x100, 0x00004550);
    Write<std::uint16_t>(image, 0x104, 0x8664);
    Write<std::uint16_t>(image, 0x114, 0xF0);
    Write<std::uint16_t>(image, 0x118, 0x20B);
    Write<std::uint32_t>(image, 0x118 + 108, 16);
    Write<std::uint32_t>(image, 0x118 + 120, 0x300);
    Write<std::uint32_t>(image, 0x118 + 124, 40);
    Write<std::uint32_t>(image, 0x300, 0x500);
    Write<std::uint32_t>(image, 0x30C, 0x400);
    Write<std::uint32_t>(image, 0x310, 0x600);
    const char module[] = "D3D11.DLL";
    std::memcpy(image.data() + 0x400, module, sizeof(module));
    Write<std::uint64_t>(image, 0x500, 0x700);
    Write<std::uint64_t>(image, 0x508, 0);
    Write<std::uint16_t>(image, 0x700, 0);
    const char symbol[] = "D3D11CreateDeviceAndSwapChain";
    std::memcpy(image.data() + 0x702, symbol, sizeof(symbol));
    Write<std::uintptr_t>(image, 0x600, 0x12345678u);
    return image;
}

bool AtomicWriter(void** slot, void* expected, void* value) noexcept
{
    std::atomic_ref<void*> atomic(*slot);
    return atomic.compare_exchange_strong(expected, value);
}

bool RejectWriter(void**, void*, void*) noexcept { return false; }

int g_originalCalls{};
int g_foreignCalls{};
int g_hookCalls{};
using ChainFunction = int (*)(int);
ChainFunction g_chainNext{};
void* g_publishedNext{};
bool g_publishedBeforeWrite{};

int Original(int value) { ++g_originalCalls; return value + 1; }
int Foreign(int value) { ++g_foreignCalls; return Original(value) + 2; }
int Hook(int value) { ++g_hookCalls; return g_chainNext(value) + 4; }

bool PublishTestNext(void* next) noexcept
{
    g_publishedNext = next;
    return true;
}

bool RequirePublishedWriter(void** slot, void* expected, void* value) noexcept
{
    g_publishedBeforeWrite = g_publishedNext == expected;
    return AtomicWriter(slot, expected, value);
}

bool StealBeforeWriter(void** slot, void*, void*) noexcept
{
    std::atomic_ref<void*> atomic(*slot);
    atomic.store(reinterpret_cast<void*>(&Foreign));
    return false;
}

struct ExactForwardEvidence
{
    void* object{};
    std::uint32_t first{};
    std::uint32_t second{};
    int calls{};
};

ExactForwardEvidence g_exactForward{};

std::int32_t ExactForward(
    void* object,
    std::uint32_t first,
    std::uint32_t second) noexcept
{
    g_exactForward = {object, first, second, g_exactForward.calls + 1};
    return static_cast<std::int32_t>(0x81234567u);
}

void TestPeIat()
{
    auto image = MakePeImage();
    const auto found = FindPe32PlusIatSlot(
        image, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    FO4HDR_CHECK(found.status == PeIatStatus::success);
    FO4HDR_CHECK(
        reinterpret_cast<std::byte*>(found.slot) - image.data() == 0x600);
    FO4HDR_CHECK(*found.slot == reinterpret_cast<void*>(0x12345678u));
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        image, "dxgi.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::moduleNotFound);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        image, "d3d11.dll", "missing").status == PeIatStatus::symbolNotFound);

    auto malformed = image;
    Write<std::uint32_t>(malformed, 0x300, 0);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::malformedImports);
    malformed = image;
    Write<std::uint16_t>(malformed, 0x104, 0x014C);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::notPe32PlusAmd64);
    malformed = image;
    Write<std::uint64_t>(malformed, 0x500, std::uint64_t{1} << 63);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::symbolNotFound);
    malformed = image;
    Write<std::uintptr_t>(malformed, 0x600, 0);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::nullTarget);
    malformed.resize(0x305);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::malformedImports);
    malformed = image;
    Write<std::uint16_t>(malformed, 0, 0);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::invalidDos);
    malformed = image;
    Write<std::uint32_t>(malformed, 0x100, 0);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::invalidNt);
    malformed = image;
    Write<std::uint32_t>(malformed, 0x118 + 124, 60);
    std::memcpy(malformed.data() + 0x314, malformed.data() + 0x300, 20);
    std::memset(malformed.data() + 0x328, 0, 20);
    FO4HDR_CHECK(FindPe32PlusIatSlot(
        malformed, "d3d11.dll", "D3D11CreateDeviceAndSwapChain").status ==
        PeIatStatus::duplicateSymbol);
}

void TestPointerOwnershipAndChain()
{
    g_originalCalls = g_foreignCalls = g_hookCalls = 0;
    void* slot = reinterpret_cast<void*>(&Foreign);
    FO4HDR_CHECK(AtomicPointerLoad(&slot) == reinterpret_cast<void*>(&Foreign));
    FO4HDR_CHECK(AtomicPointerLoad(nullptr) == nullptr);
    PointerHookOwnership ownership;
    FO4HDR_CHECK(ownership.Install(
        &slot, reinterpret_cast<void*>(&Hook), AtomicWriter));
    FO4HDR_CHECK(!ownership.Install(
        &slot, reinterpret_cast<void*>(&Hook), AtomicWriter));
    FO4HDR_CHECK(ownership.State() == HookOwnershipState::owned);
    g_chainNext = reinterpret_cast<ChainFunction>(ownership.Next());
    FO4HDR_CHECK(reinterpret_cast<ChainFunction>(slot)(10) == 17);
    FO4HDR_CHECK(g_hookCalls == 1 && g_foreignCalls == 1 && g_originalCalls == 1);

    std::atomic_ref<void*> atomic(slot);
    auto expected = reinterpret_cast<void*>(&Hook);
    FO4HDR_CHECK(atomic.compare_exchange_strong(
        expected, reinterpret_cast<void*>(&Original)));
    FO4HDR_CHECK(ownership.Health() == HookOwnershipState::ownershipLost);
    FO4HDR_CHECK(!ownership.Remove(AtomicWriter));
    FO4HDR_CHECK(slot == reinterpret_cast<void*>(&Original));
}

void TestExactForwarding()
{
    g_exactForward = {};
    int object{};
    const auto result = InvokeCapturedNext(
        &ExactForward, &object, std::uint32_t{17}, std::uint32_t{29});
    FO4HDR_CHECK(result == static_cast<std::int32_t>(0x81234567u));
    FO4HDR_CHECK(g_exactForward.object == &object);
    FO4HDR_CHECK(g_exactForward.first == 17);
    FO4HDR_CHECK(g_exactForward.second == 29);
    FO4HDR_CHECK(g_exactForward.calls == 1);
}

void TestConcurrentInstallation()
{
    void* racedSlot = reinterpret_cast<void*>(&Original);
    PointerHookOwnership racedOwner;
    std::atomic<bool> start{};
    bool firstInstalled{};
    bool secondInstalled{};
    std::thread firstThread([&] {
        while (!start.load(std::memory_order_acquire)) {}
        firstInstalled = racedOwner.Install(
            &racedSlot, reinterpret_cast<void*>(&Hook), AtomicWriter);
    });
    std::thread secondThread([&] {
        while (!start.load(std::memory_order_acquire)) {}
        secondInstalled = racedOwner.Install(
            &racedSlot, reinterpret_cast<void*>(&Hook), AtomicWriter);
    });
    start.store(true, std::memory_order_release);
    firstThread.join();
    secondThread.join();
    FO4HDR_CHECK(firstInstalled != secondInstalled);
    FO4HDR_CHECK(
        racedSlot == reinterpret_cast<void*>(&Hook));
}

void TestRejectedAndOrderedWrites()
{
    void* rejectedSlot = reinterpret_cast<void*>(&Original);
    PointerHookOwnership rejected;
    FO4HDR_CHECK(!rejected.Install(
        &rejectedSlot, reinterpret_cast<void*>(&Hook), RejectWriter));
    FO4HDR_CHECK(rejectedSlot == reinterpret_cast<void*>(&Original));
    FO4HDR_CHECK(rejected.State() == HookOwnershipState::installFailed);

    g_publishedNext = nullptr;
    g_publishedBeforeWrite = false;
    void* orderedSlot = reinterpret_cast<void*>(&Original);
    PointerHookOwnership ordered;
    FO4HDR_CHECK(ordered.Install(
        &orderedSlot,
        reinterpret_cast<void*>(&Hook),
        RequirePublishedWriter,
        PublishTestNext));
    FO4HDR_CHECK(g_publishedBeforeWrite);
    FO4HDR_CHECK(g_publishedNext == reinterpret_cast<void*>(&Original));
    FO4HDR_CHECK(!ordered.Remove(RejectWriter));
    FO4HDR_CHECK(ordered.State() == HookOwnershipState::owned);
    FO4HDR_CHECK(orderedSlot == reinterpret_cast<void*>(&Hook));
    FO4HDR_CHECK(ordered.Remove(AtomicWriter));
}

void TestForeignWriteAndSelfTargets()
{
    g_publishedNext = nullptr;
    void* stolenSlot = reinterpret_cast<void*>(&Original);
    PointerHookOwnership stolen;
    FO4HDR_CHECK(!stolen.Install(
        &stolenSlot,
        reinterpret_cast<void*>(&Hook),
        StealBeforeWriter,
        PublishTestNext));
    FO4HDR_CHECK(g_publishedNext == reinterpret_cast<void*>(&Original));
    FO4HDR_CHECK(stolenSlot == reinterpret_cast<void*>(&Foreign));
    FO4HDR_CHECK(stolen.State() == HookOwnershipState::installFailed);

    void* selfSlot = reinterpret_cast<void*>(&Hook);
    PointerHookOwnership selfOwner;
    FO4HDR_CHECK(!selfOwner.Install(
        &selfSlot, reinterpret_cast<void*>(&Hook), AtomicWriter));
    FO4HDR_CHECK(selfSlot == reinterpret_cast<void*>(&Hook));
    FO4HDR_CHECK(selfOwner.State() == HookOwnershipState::installFailed);
}

void TestInitializationAndRecordLifecyclePolicies()
{
    OneShotInitialization initialization;
    FO4HDR_CHECK(
        initialization.Begin() == OneShotInitializationAction::install);
    FO4HDR_CHECK(
        initialization.Begin() == OneShotInitializationAction::unavailable);
    initialization.Complete(true);
    FO4HDR_CHECK(
        initialization.State() == OneShotInitializationState::active);
    FO4HDR_CHECK(
        initialization.Begin() == OneShotInitializationAction::alreadyActive);

    OneShotInitialization failed;
    FO4HDR_CHECK(failed.Begin() == OneShotInitializationAction::install);
    failed.Complete(false);
    FO4HDR_CHECK(failed.State() == OneShotInitializationState::failed);
    FO4HDR_CHECK(failed.Begin() == OneShotInitializationAction::unavailable);
}

void TestConcurrentInitialization()
{
    OneShotInitialization concurrent;
    std::atomic<bool> start{};
    std::atomic<std::uint32_t> installers{};
    std::array<std::thread, 8> threads;
    for (auto& thread : threads)
    {
        thread = std::thread([&] {
            while (!start.load(std::memory_order_acquire)) {}
            if (concurrent.Begin() == OneShotInitializationAction::install)
                installers.fetch_add(1, std::memory_order_relaxed);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    FO4HDR_CHECK(installers.load(std::memory_order_relaxed) == 1);
    concurrent.Complete(true);
    FO4HDR_CHECK(
        concurrent.Begin() == OneShotInitializationAction::alreadyActive);
}

void TestBoundedLock()
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    {
        BoundedAtomicFlagGuard first(flag, 1);
        FO4HDR_CHECK(first.OwnsLock());
        BoundedAtomicFlagGuard contended(flag, 4);
        FO4HDR_CHECK(!contended.OwnsLock());
    }
    BoundedAtomicFlagGuard reacquired(flag, 1);
    FO4HDR_CHECK(reacquired.OwnsLock());
}

void TestObjectGenerationIdentity()
{
    ObjectGenerationIdentity identity;
    int firstObject{};
    int secondObject{};
    FO4HDR_CHECK(identity.Publish(&firstObject, 1));
    FO4HDR_CHECK(identity.Matches(&firstObject));
    FO4HDR_CHECK(!identity.Publish(&secondObject, 2));
    FO4HDR_CHECK(!identity.Retire(&firstObject, 2));
    FO4HDR_CHECK(!identity.Retire(&secondObject, 1));
    FO4HDR_CHECK(identity.Retire(&firstObject, 1));
    FO4HDR_CHECK(identity.Object() == nullptr);
    FO4HDR_CHECK(identity.Publish(&firstObject, 2));
    FO4HDR_CHECK(!identity.Retire(&firstObject, 1));
    FO4HDR_CHECK(identity.Retire(&firstObject, 2));

}

void TestExceptionBoundary()
{
    int calls{};
    int exceptions{};
    const auto failedResult = InvokeNoexceptBoundary<int>(
        -1,
        [&]() -> int {
            ++calls;
            throw 42;
        },
        [&]() noexcept { ++exceptions; });
    FO4HDR_CHECK(failedResult == -1);
    FO4HDR_CHECK(calls == 1 && exceptions == 1);
    const auto successResult = InvokeNoexceptBoundary<int>(
        -1,
        [&]() noexcept -> int {
            ++calls;
            return 17;
        },
        [&]() noexcept { ++exceptions; });
    FO4HDR_CHECK(successResult == 17);
    FO4HDR_CHECK(calls == 2 && exceptions == 1);
}

void TestProductionObservationPolicies()
{
    std::array<void*, 41> original{};
    std::array<void*, 41> first{};
    std::array<void*, 41> second{};
    for (std::size_t index = 0; index < original.size(); ++index)
        original[index] = reinterpret_cast<void*>(index + 1);
    const VtablePatch patches[]{
        {8, reinterpret_cast<void*>(101)},
        {13, reinterpret_cast<void*>(102)}
    };
    FO4HDR_CHECK(PrepareShadowVtable(original, first, patches));
    FO4HDR_CHECK(PrepareShadowVtable(original, second, patches));
    FO4HDR_CHECK(first.data() != second.data());
    FO4HDR_CHECK(first[8] == reinterpret_cast<void*>(101));
    FO4HDR_CHECK(first[13] == reinterpret_cast<void*>(102));
    FO4HDR_CHECK(first[40] == original[40]);
    FO4HDR_CHECK(original[8] == reinterpret_cast<void*>(9));
    auto selfOriginal = original;
    selfOriginal[8] = reinterpret_cast<void*>(101);
    FO4HDR_CHECK(!PrepareShadowVtable(selfOriginal, first, patches));
    selfOriginal = original;
    selfOriginal[13] = reinterpret_cast<void*>(102);
    FO4HDR_CHECK(!PrepareShadowVtable(selfOriginal, first, patches));
    const VtablePatch releasePatch{2, reinterpret_cast<void*>(103)};
    selfOriginal = original;
    selfOriginal[2] = reinterpret_cast<void*>(103);
    FO4HDR_CHECK(!PrepareShadowVtable(
        selfOriginal,
        first,
        std::span<const VtablePatch>(&releasePatch, 1)));
}
}  // namespace

void RunHookTests()
{
    TestPeIat();
    TestPointerOwnershipAndChain();
    TestExactForwarding();
    TestConcurrentInstallation();
    TestRejectedAndOrderedWrites();
    TestForeignWriteAndSelfTargets();
    TestInitializationAndRecordLifecyclePolicies();
    TestConcurrentInitialization();
    TestBoundedLock();
    TestObjectGenerationIdentity();
    TestExceptionBoundary();
    TestProductionObservationPolicies();
}
}  // namespace fo4hdr::tests
