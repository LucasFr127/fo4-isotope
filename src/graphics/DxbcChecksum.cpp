// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// The DXBC container checksum: the RFC 1321 MD5 block function over the bytes
// after the stored checksum, ending in the container's own final block.
#include "DxbcChecksum.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <vector>

namespace fo4hdr {
namespace {

constexpr std::size_t kChecksumEnd = 20;
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::uint32_t kMaximumChunks = 64;

// RFC 1321 section 3.4: floor(abs(sin(i + 1)) * 2^32), and each round's shifts.
constexpr std::array<std::uint32_t, 64> kSines{
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr std::array<int, 16> kShifts{7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

std::uint32_t RoundFunction(
    std::uint32_t round, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept
{
    switch (round / 16)
    {
    case 0: return (b & c) | (~b & d);
    case 1: return (d & b) | (~d & c);
    case 2: return b ^ c ^ d;
    default: return c ^ (b | ~d);
    }
}

std::uint32_t RoundWord(std::uint32_t round) noexcept
{
    constexpr std::array<std::uint32_t, 4> scale{1, 5, 3, 7}, offset{0, 1, 5, 0};
    return (scale[round / 16] * round + offset[round / 16]) % 16;
}

void CompressBlock(DxbcChecksum& state, const std::byte* block) noexcept
{
    std::array<std::uint32_t, 16> words{};
    std::memcpy(words.data(), block, sizeof(words));
    auto [a, b, c, d] = state;
    for (std::uint32_t round = 0; round < 64; ++round)
    {
        const auto mixed =
            a + RoundFunction(round, b, c, d) + kSines[round] + words[RoundWord(round)];
        const auto rotated = b + std::rotl(mixed, kShifts[round / 16 * 4 + round % 4]);
        a = d;
        d = c;
        c = b;
        b = rotated;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::uint32_t ReadWord(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void WriteWord(std::vector<std::byte>& bytes, std::size_t offset, std::size_t value) noexcept
{
    const auto word = static_cast<std::uint32_t>(value);
    std::memcpy(bytes.data() + offset, &word, sizeof(word));
}

constexpr std::uint32_t Tag(const char (&name)[5]) noexcept
{
    return static_cast<std::uint32_t>(name[0]) | static_cast<std::uint32_t>(name[1]) << 8 |
        static_cast<std::uint32_t>(name[2]) << 16 | static_cast<std::uint32_t>(name[3]) << 24;
}

bool ProgramChunk(std::uint32_t tag) noexcept
{
    constexpr std::array dropped{Tag("RDEF"), Tag("STAT"), Tag("SDBG"), Tag("SPDB"), Tag("PRIV")};
    return std::ranges::find(dropped, tag) == dropped.end();
}

struct ChunkSpan
{
    std::size_t offset{}, size{};
};

struct ProgramChunks
{
    std::array<ChunkSpan, kMaximumChunks> chunks{};
    std::size_t count{}, bytes{};
    bool dropped{};
};

std::optional<std::uint32_t> ReadChunkCount(std::span<const std::byte> container) noexcept
{
    if (container.size() < kHeaderSize || ReadWord(container, 0) != Tag("DXBC"))
        return std::nullopt;
    const auto count = ReadWord(container, 28);
    if (count > kMaximumChunks || kHeaderSize + std::size_t{4} * count > container.size())
        return std::nullopt;
    return count;
}

std::optional<ChunkSpan> ReadChunk(
    std::span<const std::byte> container, std::uint32_t index) noexcept
{
    const std::size_t offset = ReadWord(container, kHeaderSize + std::size_t{4} * index);
    if (offset > container.size() - kChunkHeaderSize) return std::nullopt;
    const auto size = kChunkHeaderSize + ReadWord(container, offset + 4);
    if (size > container.size() - offset) return std::nullopt;
    return ChunkSpan{offset, size};
}

std::optional<ProgramChunks> ReadProgramChunks(std::span<const std::byte> container) noexcept
{
    const auto count = ReadChunkCount(container);
    if (!count) return std::nullopt;
    ProgramChunks program;
    for (std::uint32_t index = 0; index < *count; ++index)
    {
        const auto chunk = ReadChunk(container, index);
        if (!chunk) return std::nullopt;
        if (!ProgramChunk(ReadWord(container, chunk->offset)))
        {
            program.dropped = true;
            continue;
        }
        program.chunks[program.count++] = *chunk;
        program.bytes += chunk->size;
    }
    return program;
}

// The header and chunk table of a container holding only these chunks, in order.
std::vector<std::byte> RebuildContainer(
    std::span<const std::byte> container, const ProgramChunks& program)
{
    const auto table = kHeaderSize + 4 * program.count;
    std::vector<std::byte> rebuilt(table + program.bytes);
    std::memcpy(rebuilt.data(), container.data(), 4);
    std::memcpy(rebuilt.data() + kChecksumEnd, container.data() + kChecksumEnd, 4);
    WriteWord(rebuilt, 24, rebuilt.size());
    WriteWord(rebuilt, 28, program.count);
    auto position = table;
    for (std::size_t index = 0; index < program.count; ++index)
    {
        const auto [offset, size] = program.chunks[index];
        WriteWord(rebuilt, kHeaderSize + 4 * index, position);
        std::memcpy(rebuilt.data() + position, container.data() + offset, size);
        position += size;
    }
    return rebuilt;
}

}  // namespace

DxbcChecksum ComputeDxbcChecksum(std::span<const std::byte> container) noexcept
{
    DxbcChecksum state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    if (container.size() < kChecksumEnd) return state;
    const auto data = container.subspan(kChecksumEnd);
    const auto full = data.size() / 64 * 64;
    for (std::size_t offset = 0; offset < full; offset += 64)
        CompressBlock(state, data.data() + offset);
    const auto rest = data.subspan(full);
    const auto count = static_cast<std::uint32_t>(data.size());
    const std::uint32_t bits = count * 8, tail = count * 2 | 1;
    // The bit count leads the last block; remaining bytes that leave no room for it get their own.
    std::array<std::byte, 64> block{};
    const std::size_t start = rest.size() < 56 ? 4 : 0;
    std::memcpy(block.data() + start, rest.data(), rest.size());
    block[start + rest.size()] = std::byte{0x80};
    if (start == 0)
    {
        CompressBlock(state, block.data());
        block.fill(std::byte{});
    }
    std::memcpy(block.data(), &bits, sizeof(bits));
    std::memcpy(block.data() + 60, &tail, sizeof(tail));
    CompressBlock(state, block.data());
    return state;
}

std::optional<DxbcChecksum> ComputeDxbcProgramChecksum(
    std::span<const std::byte> container) noexcept
{
    const auto program = ReadProgramChunks(container);
    if (!program || !program->dropped) return std::nullopt;
    try
    {
        return ComputeDxbcChecksum(RebuildContainer(container, *program));
    }
    catch (const std::bad_alloc&)
    {
        return std::nullopt;
    }
}

}  // namespace fo4hdr
