// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/PeIat.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

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

}  // namespace

namespace fo4hdr::tests {

void RunHookTests()
{
    TestNamedImport();
    TestPeHeaderBounds();
    TestImportBounds();
    TestNamesAndAlignment();
    TestOrdinalAndAmbiguity();
}

}  // namespace fo4hdr::tests
