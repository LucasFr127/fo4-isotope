// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/PeIat.hpp"
#include "Fallout4HDR/hooks/HookOwnership.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <Windows.h>

namespace {

using Status = fo4hdr::PeIatStatus;
constexpr std::size_t nt = 0x80;
constexpr std::size_t optional = nt + 24;
constexpr std::size_t imports = 0x200;
constexpr std::size_t moduleName = 0x280;
constexpr std::size_t names = 0x300;
constexpr std::size_t slots = 0x340;
constexpr std::size_t symbolName = 0x380;
constexpr std::string_view symbol = "D3D11CreateDeviceAndSwapChain";
constexpr std::uint64_t resolvedTarget = 0x1234567812345678ULL;

struct alignas(void*) PeImage
{
    std::array<std::byte, 0x800> bytes{};
};

template <class T>
void Write(PeImage& image, std::size_t offset, T value)
{
    std::memcpy(image.bytes.data() + offset, &value, sizeof(value));
}

PeImage MakePeImage()
{
    PeImage image;
    Write<std::uint16_t>(image, 0, 0x5A4D);
    Write<std::int32_t>(image, 0x3C, nt);
    Write<std::uint32_t>(image, nt, 0x00004550);
    Write<std::uint16_t>(image, nt + 4, 0x8664);
    Write<std::uint16_t>(image, nt + 20, 0xF0);
    Write<std::uint16_t>(image, optional, 0x20B);
    Write<std::uint32_t>(image, optional + 108, 16);
    Write<std::uint32_t>(image, optional + 120, imports);
    Write<std::uint32_t>(image, optional + 124, 40);
    Write<std::uint32_t>(image, imports, names);
    Write<std::uint32_t>(image, imports + 12, moduleName);
    Write<std::uint32_t>(image, imports + 16, slots);
    const char module[] = "D3d11.dLl";
    std::memcpy(image.bytes.data() + moduleName, module, sizeof(module));
    Write<std::uint64_t>(image, names, symbolName);
    Write<std::uint64_t>(image, slots, resolvedTarget);
    std::memcpy(image.bytes.data() + symbolName + 2, symbol.data(), symbol.size());
    return image;
}

fo4hdr::PeIatResult CheckLookup(PeImage& image, Status expected,
    std::string_view module = "d3d11.dll", std::string_view name = symbol)
{
    const auto before = image.bytes;
    const auto result = fo4hdr::FindPe32PlusIatSlot(image.bytes, module, name);
    FO4HDR_CHECK(result.status == expected);
    FO4HDR_CHECK(image.bytes == before);
    if (expected != Status::success)
    {
        FO4HDR_CHECK(result.slot == nullptr);
    }
    return result;
}

void TestNamedImport()
{
    auto image = MakePeImage();
    const auto result = CheckLookup(image, Status::success);
    FO4HDR_CHECK(reinterpret_cast<std::byte*>(result.slot) == image.bytes.data() + slots);
    std::uint64_t target{};
    std::memcpy(&target, image.bytes.data() + slots, sizeof(target));
    FO4HDR_CHECK(target == resolvedTarget);
    static_cast<void>(CheckLookup(image, Status::success, "D3D11.DLL"));
    static_cast<void>(CheckLookup(image, Status::moduleNotFound, "dxgi.dll"));
    static_cast<void>(CheckLookup(image, Status::symbolNotFound, "d3d11.dll", "missing"));
    static_cast<void>(CheckLookup(image, Status::symbolNotFound, "d3d11.dll",
        "d3d11CreateDeviceAndSwapChain"));
    auto copy = image;
    const auto copied = CheckLookup(copy, Status::success);
    FO4HDR_CHECK(reinterpret_cast<std::byte*>(copied.slot) == copy.bytes.data() + slots);
    FO4HDR_CHECK(copied.slot != result.slot);
    Write<std::uint16_t>(copy, nt + 20, 128);
    Write<std::uint32_t>(copy, optional + 108, 2);
    static_cast<void>(CheckLookup(copy, Status::success));
}

using Owner = fo4hdr::PointerHookOwnership;
using OwnerState = fo4hdr::HookOwnershipState;
using ChainFunction = int (*)(int) noexcept;
std::atomic<void*> publishedNext{};
std::atomic<unsigned> publications{};
unsigned originalCalls{};
unsigned foreignCalls{};
unsigned replacementCalls{};

int Original(int value) noexcept
{
    ++originalCalls;
    return value + 1;
}

int Foreign(int value) noexcept
{
    ++foreignCalls;
    return Original(value) + 2;
}

int Replacement(int value) noexcept
{
    ++replacementCalls;
    const auto next = reinterpret_cast<ChainFunction>(publishedNext.load());
    return next(value) + 4;
}

void* OriginalPointer() noexcept { return reinterpret_cast<void*>(&Original); }
void* ForeignPointer() noexcept { return reinterpret_cast<void*>(&Foreign); }
void* ReplacementPointer() noexcept { return reinterpret_cast<void*>(&Replacement); }

bool PublishNext(void* next) noexcept
{
    publishedNext.store(next, std::memory_order_release);
    publications.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RejectPublication(void*) noexcept { return false; }
bool RejectWrite(void**, void*, void*) noexcept { return false; }

void* ComparePointer(void** slot, void* expected, void* value) noexcept
{
    std::atomic_ref<void*> pointer(*slot);
    static_cast<void>(pointer.compare_exchange_strong(expected, value));
    return expected;
}

bool OrderedWrite(void** slot, void* expected, void* value) noexcept
{
    if (publishedNext.load(std::memory_order_acquire) != expected)
    {
        return false;
    }
    return ComparePointer(slot, expected, value) == expected;
}

bool AtomicWrite(void** slot, void* expected, void* value) noexcept
{
    return ComparePointer(slot, expected, value) == expected;
}

bool InstallOwner(Owner& owner, void** slot, fo4hdr::HookPointerWriter writer = OrderedWrite)
{
    return owner.Install(slot, ReplacementPointer(), writer, PublishNext);
}

void TestCooperativeOwnership()
{
    originalCalls = foreignCalls = replacementCalls = 0;
    publications = 0;
    publishedNext = nullptr;
    void* slot = ForeignPointer();
    Owner owner;
    FO4HDR_CHECK(InstallOwner(owner, &slot));
    FO4HDR_CHECK(owner.Next() == ForeignPointer());
    FO4HDR_CHECK(publications == 1);
    FO4HDR_CHECK(owner.Health() == OwnerState::owned);
    FO4HDR_CHECK(reinterpret_cast<ChainFunction>(slot)(10) == 17);
    FO4HDR_CHECK(originalCalls == 1);
    FO4HDR_CHECK(foreignCalls == 1);
    FO4HDR_CHECK(replacementCalls == 1);
    FO4HDR_CHECK(!InstallOwner(owner, &slot));
    FO4HDR_CHECK(owner.Remove(AtomicWrite));
    FO4HDR_CHECK(owner.State() == OwnerState::restored);
    FO4HDR_CHECK(slot == ForeignPointer());
    FO4HDR_CHECK(owner.Next() == ForeignPointer());
    FO4HDR_CHECK(!owner.Remove(AtomicWrite));
    FO4HDR_CHECK(!InstallOwner(owner, &slot));
}

void TestForeignDisplacement()
{
    void* slot = OriginalPointer();
    Owner owner;
    FO4HDR_CHECK(InstallOwner(owner, &slot));
    FO4HDR_CHECK(AtomicWrite(&slot, ReplacementPointer(), ForeignPointer()));
    FO4HDR_CHECK(owner.Health() == OwnerState::ownershipLost);
    FO4HDR_CHECK(!owner.Remove(AtomicWrite));
    FO4HDR_CHECK(slot == ForeignPointer());
    FO4HDR_CHECK(owner.Next() == OriginalPointer());
    FO4HDR_CHECK(!InstallOwner(owner, &slot));
    Owner rejected;
    slot = OriginalPointer();
    FO4HDR_CHECK(!InstallOwner(rejected, &slot, RejectWrite));
    FO4HDR_CHECK(slot == OriginalPointer());
    FO4HDR_CHECK(rejected.Next() == OriginalPointer());
    FO4HDR_CHECK(rejected.State() == OwnerState::installFailed);
}

void TestOwnershipRefusals()
{
    for (const auto candidate : {static_cast<void*>(nullptr), ReplacementPointer()})
    {
        void* slot = candidate;
        Owner owner;
        FO4HDR_CHECK(!InstallOwner(owner, &slot));
        FO4HDR_CHECK(slot == candidate);
        FO4HDR_CHECK(owner.State() == OwnerState::installFailed);
    }
    Owner nullSlot;
    FO4HDR_CHECK(!InstallOwner(nullSlot, nullptr));
    void* slot = OriginalPointer();
    Owner refusedPublication;
    FO4HDR_CHECK(!refusedPublication.Install(&slot, ReplacementPointer(), AtomicWrite, RejectPublication));
    FO4HDR_CHECK(slot == OriginalPointer());
    FO4HDR_CHECK(refusedPublication.Next() == OriginalPointer());
    Owner noWriter;
    FO4HDR_CHECK(!InstallOwner(noWriter, &slot, nullptr));
    Owner nullReplacement;
    FO4HDR_CHECK(!nullReplacement.Install(&slot, nullptr, AtomicWrite, PublishNext));
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(nullptr) == nullptr);
}

void TestFailedRemovalKeepsNext()
{
    void* slot = OriginalPointer();
    Owner owner;
    FO4HDR_CHECK(InstallOwner(owner, &slot));
    FO4HDR_CHECK(!owner.Remove(RejectWrite));
    FO4HDR_CHECK(owner.State() == OwnerState::owned);
    FO4HDR_CHECK(owner.Next() == OriginalPointer());
    FO4HDR_CHECK(reinterpret_cast<ChainFunction>(slot)(10) == 15);
    FO4HDR_CHECK(owner.Remove(AtomicWrite));
    FO4HDR_CHECK(slot == OriginalPointer());
}

void TestOneShotAndIdentity()
{
    fo4hdr::OneShotInitialization active;
    FO4HDR_CHECK(active.Begin() == fo4hdr::OneShotInitializationAction::install);
    FO4HDR_CHECK(active.Begin() == fo4hdr::OneShotInitializationAction::unavailable);
    active.Complete(true);
    FO4HDR_CHECK(active.Begin() == fo4hdr::OneShotInitializationAction::alreadyActive);
    fo4hdr::OneShotInitialization failed;
    FO4HDR_CHECK(failed.Begin() == fo4hdr::OneShotInitializationAction::install);
    failed.Complete(false);
    FO4HDR_CHECK(failed.Begin() == fo4hdr::OneShotInitializationAction::unavailable);
    fo4hdr::ObjectGenerationIdentity identity;
    int object{};
    FO4HDR_CHECK(!identity.Publish(&object, 0));
    FO4HDR_CHECK(identity.Publish(&object, 7));
    FO4HDR_CHECK(identity.Matches(&object));
    FO4HDR_CHECK(!identity.Retire(&object, 6));
    FO4HDR_CHECK(identity.Retire(&object, 7));
    FO4HDR_CHECK(!identity.Matches(&object));
}

} // namespace

namespace fo4hdr::tests {
void RunHookTests()
{
    TestNamedImport();
    TestCooperativeOwnership();
    TestOwnershipRefusals();
    TestForeignDisplacement();
    TestFailedRemovalKeepsNext();
    TestOneShotAndIdentity();
}
} // namespace fo4hdr::tests
