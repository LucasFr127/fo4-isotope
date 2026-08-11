// SPDX-License-Identifier: GPL-3.0-or-later

#include "hooks/NativeImageSlots.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstring>

namespace {

using Image = std::array<std::byte, 0xA00>;
constexpr std::uintptr_t preferredBase = 0x180000000;
constexpr std::uintptr_t relocatedBase = 0x280000000;

template <class Value>
void Write(Image& image, std::size_t offset, const Value& value)
{
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

Image StaticSlotImage()
{
    Image image{};
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    Write(image, 0, dos);
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 3;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.Characteristics = IMAGE_FILE_DLL;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.ImageBase = preferredBase;
    nt.OptionalHeader.SizeOfHeaders = 0x400;
    nt.OptionalHeader.SizeOfImage = 0x4000;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {0x3000, 12};
    Write(image, 0x80, nt);
    std::array<IMAGE_SECTION_HEADER, 3> sections{};
    for (std::size_t index = 0; index < sections.size(); ++index)
    {
        sections[index].VirtualAddress = static_cast<DWORD>(0x1000 * (index + 1));
        sections[index].Misc.VirtualSize = 0x200;
        sections[index].SizeOfRawData = 0x200;
        sections[index].PointerToRawData = static_cast<DWORD>(0x400 + 0x200 * index);
        sections[index].Characteristics = IMAGE_SCN_MEM_READ;
    }
    sections[0].Characteristics |= IMAGE_SCN_MEM_EXECUTE;
    Write(image, 0x80 + sizeof(nt), sections);
    Write(image, 0x600, std::uint64_t{preferredBase + 0x1010});
    const IMAGE_BASE_RELOCATION block{0x2000, 12};
    Write(image, 0x800, block);
    Write(image, 0x808, WORD{IMAGE_REL_BASED_DIR64 << 12});
    return image;
}

}  // namespace

namespace fo4hdr::tests {

void RunNativeImageSlotsTests()
{
    auto image = StaticSlotImage();
    const auto original = image;
    fo4hdr::NativeImageSlots slots;
    FO4HDR_CHECK(slots.Parse(image));
    FO4HDR_CHECK(slots.Expected(relocatedBase, 0x2000) == reinterpret_cast<void*>(relocatedBase + 0x1010));
    FO4HDR_CHECK(image == original);
    // Removing the independent relocation evidence forbids the relocated admission.
    Write(image, 0x808, WORD{});
    FO4HDR_CHECK(slots.Parse(image));
    FO4HDR_CHECK(slots.Expected(relocatedBase, 0x2000) == nullptr);
    FO4HDR_CHECK(slots.Expected(preferredBase, 0x2000) == reinterpret_cast<void*>(preferredBase + 0x1010));
}

}  // namespace fo4hdr::tests
