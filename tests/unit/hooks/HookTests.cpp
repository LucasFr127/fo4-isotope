// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/PeIat.hpp"
#include "Fallout4HDR/hooks/HookOwnership.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <thread>

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

template <class T>
void CheckField(std::size_t offset, T value, Status expected)
{
    auto image = MakePeImage();
    Write(image, offset, value);
    static_cast<void>(CheckLookup(image, expected));
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

void TestPeHeaderBounds()
{
    auto image = MakePeImage();
    FO4HDR_CHECK(fo4hdr::FindPe32PlusIatSlot({}, "d3d11.dll", symbol).status ==
        Status::invalidArgument);
    static_cast<void>(CheckLookup(image, Status::invalidArgument, ""));
    static_cast<void>(CheckLookup(image, Status::invalidArgument, "d3d11.dll", ""));
    for (const auto extent : {std::size_t{1}, std::size_t{0x3F}})
    {
        FO4HDR_CHECK(fo4hdr::FindPe32PlusIatSlot(std::span{image.bytes}.first(extent),
            "d3d11.dll", symbol).status == Status::invalidDos);
    }
    for (const auto extent : {nt + 23, optional + 0xEF})
    {
        FO4HDR_CHECK(fo4hdr::FindPe32PlusIatSlot(std::span{image.bytes}.first(extent),
            "d3d11.dll", symbol).status == Status::invalidNt);
    }
    CheckField<std::uint16_t>(0, 0, Status::invalidDos);
    CheckField<std::int32_t>(0x3C, -1, Status::invalidNt);
    CheckField<std::int32_t>(0x3C, 0x20, Status::invalidNt);
    CheckField<std::int32_t>(0x3C, 0x7FFFFFFF, Status::invalidNt);
    CheckField<std::uint32_t>(nt, 0, Status::invalidNt);
    CheckField<std::uint16_t>(nt + 4, 0x014C, Status::notPe32PlusAmd64);
    CheckField<std::uint16_t>(optional, 0x10B, Status::notPe32PlusAmd64);
    CheckField<std::uint16_t>(nt + 20, 111, Status::invalidNt);
    CheckField<std::uint16_t>(nt + 20, 0xFFFF, Status::invalidNt);
    CheckField<std::uint32_t>(optional + 108, 17, Status::invalidNt);
    CheckField<std::uint32_t>(optional + 108, 1, Status::importsUnavailable);
}

void TestImportBounds()
{
    CheckField<std::uint32_t>(optional + 120, 0, Status::importsUnavailable);
    CheckField<std::uint32_t>(optional + 124, 0, Status::importsUnavailable);
    CheckField<std::uint32_t>(optional + 124, 19, Status::malformedImports);
    CheckField<std::uint32_t>(optional + 124, 20, Status::malformedImports);
    CheckField<std::uint32_t>(optional + 120, 0xFFFFFFFF, Status::malformedImports);
    CheckField<std::uint32_t>(optional + 124, 0xFFFFFFFF, Status::malformedImports);
    CheckField<std::uint32_t>(imports, 0, Status::malformedImports);
    CheckField<std::uint32_t>(imports + 12, 0x800, Status::malformedImports);
    CheckField<std::uint32_t>(imports + 16, 0, Status::malformedImports);
    CheckField<std::uint32_t>(imports + 20 + 4, 1, Status::malformedImports);
    CheckField<std::uint32_t>(imports, 0x7F9, Status::malformedImports);
    CheckField<std::uint32_t>(imports + 16, 0x7F9, Status::malformedImports);
    CheckField<std::uint64_t>(names, std::uint64_t{1} << 62, Status::malformedImports);
    CheckField<std::uint64_t>(names, 0x7FF, Status::malformedImports);
    CheckField<std::uint64_t>(names, 0x7FE, Status::malformedImports);
    CheckField<std::uint64_t>(slots + 8, 1, Status::malformedImports);
    auto image = MakePeImage();
    Write<std::uint32_t>(image, imports, 0x7F8);
    Write<std::uint32_t>(image, imports + 16, 0x7F0);
    Write<std::uint64_t>(image, 0x7F8, symbolName);
    Write<std::uint64_t>(image, 0x7F0, 1);
    static_cast<void>(CheckLookup(image, Status::malformedImports));
}

void TestNamesAndAlignment()
{
    for (const auto offset : {moduleName, symbolName + 2})
    {
        auto image = MakePeImage();
        std::fill(image.bytes.begin() + offset, image.bytes.end(), std::byte{'X'});
        static_cast<void>(CheckLookup(image, Status::malformedImports));
    }
    CheckField<std::uint32_t>(imports + 16, slots + 1, Status::malformedImports);
    const auto image = MakePeImage();
    alignas(void*) std::array<std::byte, 0x801> shifted{};
    std::memcpy(shifted.data() + 1, image.bytes.data(), image.bytes.size());
    const auto before = shifted;
    const auto result = fo4hdr::FindPe32PlusIatSlot(
        std::span{shifted}.subspan(1), "d3d11.dll", symbol);
    FO4HDR_CHECK(result.status == Status::malformedImports);
    FO4HDR_CHECK(result.slot == nullptr);
    FO4HDR_CHECK(shifted == before);
}

void TestOrdinalAndAmbiguity()
{
    auto image = MakePeImage();
    Write<std::uint64_t>(image, names, (std::uint64_t{1} << 63) | 7);
    static_cast<void>(CheckLookup(image, Status::symbolNotFound));
    Write<std::uint64_t>(image, names + 8, symbolName);
    Write<std::uint64_t>(image, slots + 8, 1);
    const auto result = CheckLookup(image, Status::success);
    FO4HDR_CHECK(reinterpret_cast<std::byte*>(result.slot) == image.bytes.data() + slots + 8);
    CheckField<std::uint64_t>(slots, 0, Status::nullTarget);
    image = MakePeImage();
    Write<std::uint64_t>(image, names + 8, symbolName);
    Write<std::uint64_t>(image, slots + 8, 1);
    static_cast<void>(CheckLookup(image, Status::duplicateSymbol));
    image = MakePeImage();
    Write<std::uint32_t>(image, optional + 124, 60);
    std::memcpy(image.bytes.data() + imports + 20, image.bytes.data() + imports, 20);
    static_cast<void>(CheckLookup(image, Status::duplicateSymbol));
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

void TestOwnershipRefusals()
{
    publications = 0;
    void* slot = OriginalPointer();
    for (const auto candidate : {static_cast<void*>(nullptr), ReplacementPointer()})
    {
        slot = candidate;
        Owner owner;
        FO4HDR_CHECK(!InstallOwner(owner, &slot));
        FO4HDR_CHECK(slot == candidate);
        FO4HDR_CHECK(owner.State() == OwnerState::installFailed);
    }
    Owner nullSlot;
    FO4HDR_CHECK(!InstallOwner(nullSlot, nullptr));
    slot = OriginalPointer();
    Owner rejectedTarget;
    void* uncallable = &slot;
    FO4HDR_CHECK(!rejectedTarget.Install(&uncallable, ReplacementPointer(), AtomicWrite,
        PublishNext));
    FO4HDR_CHECK(slot == OriginalPointer());
    Owner refusedPublication;
    FO4HDR_CHECK(!refusedPublication.Install(&slot, ReplacementPointer(), AtomicWrite,
        RejectPublication));
    FO4HDR_CHECK(slot == OriginalPointer());
    FO4HDR_CHECK(refusedPublication.Next() == OriginalPointer());
    Owner noWriter;
    FO4HDR_CHECK(!InstallOwner(noWriter, &slot, nullptr));
    Owner noPublisher;
    FO4HDR_CHECK(!noPublisher.Install(&slot, ReplacementPointer(), AtomicWrite,
        nullptr));
    Owner nullReplacement;
    FO4HDR_CHECK(!nullReplacement.Install(&slot, nullptr, AtomicWrite,
        PublishNext));
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(nullptr) == nullptr);
    FO4HDR_CHECK(!fo4hdr::IsExecutableHookTarget(nullptr));
    FO4HDR_CHECK(!fo4hdr::IsExecutableHookTarget(&slot));
    FO4HDR_CHECK(publications == 0);
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

void TestFailedRemovalKeepsNext()
{
    void* slot = OriginalPointer();
    Owner owner;
    FO4HDR_CHECK(InstallOwner(owner, &slot));
    FO4HDR_CHECK(!owner.Remove(RejectWrite));
    FO4HDR_CHECK(owner.State() == OwnerState::passive);
    FO4HDR_CHECK(owner.Next() == OriginalPointer());
    FO4HDR_CHECK(reinterpret_cast<ChainFunction>(slot)(10) == 15);
    FO4HDR_CHECK(!owner.Remove(AtomicWrite));
    FO4HDR_CHECK(AtomicWrite(&slot, ReplacementPointer(), OriginalPointer()));
}

enum class WriteFault
{
    none,
    refuseProtection,
    refuseRestoration,
    persistentRestoration,
    foreignBeforeWrite,
    foreignBeforeRollback,
    reachableAfterRollback
};

struct WriteEvidence
{
    WriteFault fault{};
    std::uint32_t protection{PAGE_READONLY};
    unsigned protects{};
    unsigned exchanges{};
};
WriteEvidence writeEvidence;

bool ShouldRefuseProtection(unsigned call, WriteFault fault) noexcept
{
    switch (call)
    {
    case 1:
        return fault == WriteFault::refuseProtection;
    case 2:
        return fault != WriteFault::none && fault != WriteFault::foreignBeforeWrite;
    case 3:
        return fault == WriteFault::persistentRestoration;
    default:
        return false;
    }
}

bool FaultProtection(void*, std::size_t bytes, std::uint32_t protection,
    std::uint32_t& previous) noexcept
{
    ++writeEvidence.protects;
    previous = writeEvidence.protection;
    if (bytes != sizeof(void*))
    {
        return false;
    }
    if (ShouldRefuseProtection(writeEvidence.protects, writeEvidence.fault))
    {
        return false;
    }
    writeEvidence.protection = protection;
    return true;
}

void* FaultExchange(void** slot, void* expected, void* value) noexcept
{
    ++writeEvidence.exchanges;
    if (writeEvidence.exchanges == 1 &&
        writeEvidence.fault == WriteFault::foreignBeforeWrite)
    {
        std::atomic_ref<void*>(*slot).store(ForeignPointer());
    }
    if (writeEvidence.exchanges == 2)
    {
        if (writeEvidence.fault == WriteFault::foreignBeforeRollback ||
            writeEvidence.fault == WriteFault::reachableAfterRollback)
        {
            std::atomic_ref<void*>(*slot).store(ForeignPointer());
        }
    }
    const auto observed = ComparePointer(slot, expected, value);
    if (writeEvidence.exchanges == 2 &&
        writeEvidence.fault == WriteFault::reachableAfterRollback)
    {
        std::atomic_ref<void*>(*slot).store(ReplacementPointer());
    }
    return observed;
}

bool ProtectedFaultWrite(void** slot, void* expected, void* value) noexcept
{
    const fo4hdr::HookPointerWriteOperations operations{FaultProtection, FaultExchange};
    return fo4hdr::WriteProtectedPointer(slot, expected, value, operations);
}

void TestProtectionTransaction()
{
    for (const auto fault : {WriteFault::none, WriteFault::refuseProtection,
             WriteFault::refuseRestoration, WriteFault::persistentRestoration,
             WriteFault::foreignBeforeWrite, WriteFault::foreignBeforeRollback})
    {
        writeEvidence = {fault};
        void* slot = OriginalPointer();
        const auto written = ProtectedFaultWrite(&slot, OriginalPointer(), ReplacementPointer());
        FO4HDR_CHECK(written == (fault == WriteFault::none));
        const auto expected = fault == WriteFault::none ? ReplacementPointer() : OriginalPointer();
        if (fault == WriteFault::foreignBeforeWrite || fault == WriteFault::foreignBeforeRollback)
        {
            FO4HDR_CHECK(slot == ForeignPointer());
        }
        else
        {
            FO4HDR_CHECK(slot == expected);
        }
        if (fault == WriteFault::refuseProtection)
        {
            FO4HDR_CHECK(writeEvidence.exchanges == 0);
        }
        if (fault == WriteFault::persistentRestoration)
        {
            FO4HDR_CHECK(writeEvidence.protection == PAGE_READWRITE);
        }
        else
        {
            FO4HDR_CHECK(writeEvidence.protection == PAGE_READONLY);
        }
    }
}

void TestInvalidWriteOperations()
{
    void* slot = OriginalPointer();
    const fo4hdr::HookPointerWriteOperations noProtection{nullptr, FaultExchange};
    const fo4hdr::HookPointerWriteOperations noExchange{FaultProtection, nullptr};
    FO4HDR_CHECK(!fo4hdr::WriteProtectedPointer(&slot, OriginalPointer(),
        ReplacementPointer(), noProtection));
    FO4HDR_CHECK(!fo4hdr::WriteProtectedPointer(&slot, OriginalPointer(),
        ReplacementPointer(), noExchange));
    FO4HDR_CHECK(!fo4hdr::WriteProtectedPointer(nullptr, OriginalPointer(), ReplacementPointer()));
    alignas(void*) std::array<std::byte, sizeof(void*) + 1> unaligned{};
    auto** address = reinterpret_cast<void**>(unaligned.data() + 1);
    FO4HDR_CHECK(!fo4hdr::WriteProtectedPointer(address, OriginalPointer(), ReplacementPointer()));
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(address) == nullptr);
    Owner owner;
    FO4HDR_CHECK(!InstallOwner(owner, address));
    FO4HDR_CHECK(slot == OriginalPointer());
}

void TestReachableFailedHook()
{
    writeEvidence = {WriteFault::reachableAfterRollback};
    publishedNext = nullptr;
    void* slot = ForeignPointer();
    Owner owner;
    FO4HDR_CHECK(!InstallOwner(owner, &slot, ProtectedFaultWrite));
    FO4HDR_CHECK(slot == ReplacementPointer());
    FO4HDR_CHECK(owner.State() == OwnerState::passive);
    FO4HDR_CHECK(owner.Next() == ForeignPointer());
    FO4HDR_CHECK(publishedNext == ForeignPointer());
    FO4HDR_CHECK(reinterpret_cast<ChainFunction>(slot)(10) == 17);
    FO4HDR_CHECK(!owner.Remove(AtomicWrite));
    FO4HDR_CHECK(!InstallOwner(owner, &slot));
    FO4HDR_CHECK(slot == ReplacementPointer());
    FO4HDR_CHECK(AtomicWrite(&slot, ReplacementPointer(), ForeignPointer()));
}

void TestConcurrentOwnershipClaim()
{
    publications = 0;
    publishedNext = nullptr;
    void* slot = OriginalPointer();
    Owner owner;
    std::barrier start(2);
    bool workerInstalled{};
    std::jthread worker([&] {
        start.arrive_and_wait();
        workerInstalled = InstallOwner(owner, &slot);
    });
    start.arrive_and_wait();
    const auto mainInstalled = InstallOwner(owner, &slot);
    worker.join();
    FO4HDR_CHECK(mainInstalled != workerInstalled);
    FO4HDR_CHECK(publications == 1);
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(&slot) == ReplacementPointer());
    FO4HDR_CHECK(owner.State() == OwnerState::owned);
    FO4HDR_CHECK(owner.Next() == OriginalPointer());
    FO4HDR_CHECK(owner.Remove(AtomicWrite));
}

struct OwnedPointerPage
{
    OwnedPointerPage() = default;
    OwnedPointerPage(const OwnedPointerPage&) = delete;
    OwnedPointerPage& operator=(const OwnedPointerPage&) = delete;

    void* address{VirtualAlloc(nullptr, sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)};
    ~OwnedPointerPage()
    {
        if (address != nullptr)
        {
            FO4HDR_CHECK(VirtualFree(address, 0, MEM_RELEASE) != FALSE);
        }
    }
};

void TestNativeOwnedPage()
{
    OwnedPointerPage page;
    FO4HDR_CHECK(page.address != nullptr);
    if (page.address == nullptr)
    {
        return;
    }
    auto** slot = ::new (page.address) void*(OriginalPointer());
    DWORD previous{};
    FO4HDR_CHECK(VirtualProtect(slot, sizeof(void*), PAGE_READONLY, &previous) != FALSE);
    FO4HDR_CHECK(previous == PAGE_READWRITE);
    Owner owner;
    FO4HDR_CHECK(InstallOwner(owner, slot, fo4hdr::WriteProtectedPointer));
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(slot) == ReplacementPointer());
    MEMORY_BASIC_INFORMATION information{};
    FO4HDR_CHECK(VirtualQuery(slot, &information, sizeof(information)) == sizeof(information));
    FO4HDR_CHECK(information.Protect == PAGE_READONLY);
    FO4HDR_CHECK(owner.Remove(fo4hdr::WriteProtectedPointer));
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(slot) == OriginalPointer());
    FO4HDR_CHECK(VirtualQuery(slot, &information, sizeof(information)) == sizeof(information));
    FO4HDR_CHECK(information.Protect == PAGE_READONLY);
}

}  // namespace

namespace fo4hdr::tests {

void RunHookTests()
{
    TestNamedImport();
    TestPeHeaderBounds();
    TestImportBounds();
    TestNamesAndAlignment();
    TestOrdinalAndAmbiguity();
    TestCooperativeOwnership();
    TestOwnershipRefusals();
    TestForeignDisplacement();
    TestFailedRemovalKeepsNext();
    TestProtectionTransaction();
    TestInvalidWriteOperations();
    TestReachableFailedHook();
    TestConcurrentOwnershipClaim();
    TestNativeOwnedPage();
}

}  // namespace fo4hdr::tests
