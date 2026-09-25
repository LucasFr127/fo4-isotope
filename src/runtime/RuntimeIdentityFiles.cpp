// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "RuntimeIdentityFiles.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace fo4hdr {
namespace {

class AlgorithmHandle
{
public:
    ~AlgorithmHandle()
    {
        if (value != nullptr)
        {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }

    BCRYPT_ALG_HANDLE value{};
};

class HashHandle
{
public:
    ~HashHandle()
    {
        if (value != nullptr)
        {
            BCryptDestroyHash(value);
        }
    }

    BCRYPT_HASH_HANDLE value{};
};

[[nodiscard]] bool ReadHashProperty(
    BCRYPT_ALG_HANDLE algorithm, LPCWSTR property, DWORD& value) noexcept
{
    DWORD resultSize{};
    return BCryptGetProperty(algorithm, property,
        reinterpret_cast<PUCHAR>(&value), sizeof(value), &resultSize, 0) == 0;
}

[[nodiscard]] bool HasUtf8Bom(std::span<const unsigned char> bytes) noexcept
{
    constexpr std::array<unsigned char, 3> bom{0xEF, 0xBB, 0xBF};
    return bytes.size() >= bom.size() &&
        std::equal(bom.begin(), bom.end(), bytes.begin());
}

[[nodiscard]] std::optional<std::vector<unsigned char>> ReadCanonicalText(
    std::istream& stream)
{
    std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>{});
    if (stream.bad() || bytes.empty()) return std::nullopt;
    if (bytes.back() != '\n' || HasUtf8Bom(bytes)) return std::nullopt;

    // The final LF rules out a trailing CR; every other CR must belong to CRLF.
    const auto bareCr = std::adjacent_find(bytes.begin(), bytes.end(),
        [](unsigned char current, unsigned char next) noexcept {
            return current == '\r' && next != '\n';
        });
    if (bareCr != bytes.end()) return std::nullopt;
    std::erase(bytes, '\r');
    return bytes;
}

[[nodiscard]] std::string FormatSha256(std::span<const UCHAR> digest)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        result[index * 2] = hex[digest[index] >> 4];
        result[index * 2 + 1] = hex[digest[index] & 0x0F];
    }
    return result;
}

[[nodiscard]] std::optional<std::string> ComputeSha256(const auto& feed)
{
    AlgorithmHandle algorithm;
    if (BCryptOpenAlgorithmProvider(
            &algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return std::nullopt;

    DWORD objectSize = 0;
    DWORD hashSize = 0;
    if (!ReadHashProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, objectSize) ||
        !ReadHashProperty(algorithm.value, BCRYPT_HASH_LENGTH, hashSize))
    {
        return std::nullopt;
    }
    if (hashSize != 32) return std::nullopt;

    std::vector<UCHAR> object(objectSize);
    HashHandle hash;
    if (BCryptCreateHash(
            algorithm.value,
            &hash.value,
            object.data(),
            objectSize,
            nullptr,
            0,
            0) != 0 ||
        !feed(hash.value))
    {
        return std::nullopt;
    }

    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(
            hash.value,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0) != 0)
    {
        return std::nullopt;
    }

    return FormatSha256(digest);
}

}  // namespace

[[nodiscard]] std::optional<std::string> Sha256File(
    const std::filesystem::path& path) noexcept
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }

        return ComputeSha256(
            [&](BCRYPT_HASH_HANDLE hash) noexcept {
                std::array<char, 64 * 1024> buffer{};
                while (stream)
                {
                    stream.read(buffer.data(), buffer.size());
                    const auto count = stream.gcount();
                    if (count > 0 &&
                        BCryptHashData(
                            hash,
                            reinterpret_cast<PUCHAR>(buffer.data()),
                            static_cast<ULONG>(count),
                            0) != 0)
                    {
                        return false;
                    }
                }
                return stream.eof() && !stream.bad();
            });
    }
    catch (...)
    {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string> Sha256CanonicalTextFile(
    const std::filesystem::path& path) noexcept
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }

        auto canonical = ReadCanonicalText(stream);
        if (!canonical) return std::nullopt;

        return ComputeSha256(
            [&](BCRYPT_HASH_HANDLE hash) noexcept {
                return BCryptHashData(
                    hash,
                    canonical->data(),
                    static_cast<ULONG>(canonical->size()),
                    0) == 0;
            });
    }
    catch (...)
    {
        return std::nullopt;
    }
}

}  // namespace fo4hdr
