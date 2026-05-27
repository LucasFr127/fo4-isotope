// SPDX-License-Identifier: GPL-3.0-or-later

#include "RuntimeIdentityFiles.hpp"

#include <Windows.h>
#include <winver.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fo4hdr {
namespace {

[[nodiscard]] std::optional<RuntimeVersion> ReadVersionResource(
    std::span<const std::byte> resource) noexcept
{
    void* fixedInfo{};
    UINT fixedSize{};
    if (!VerQueryValueW(resource.data(), L"\\", &fixedInfo, &fixedSize))
    {
        return std::nullopt;
    }
    if (fixedInfo == nullptr || fixedSize != sizeof(VS_FIXEDFILEINFO))
    {
        return std::nullopt;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(resource.data());
    const auto address = reinterpret_cast<std::uintptr_t>(fixedInfo);
    if (address < begin)
    {
        return std::nullopt;
    }
    const auto offset = address - begin;
    if (offset > resource.size())
    {
        return std::nullopt;
    }
    if (fixedSize > resource.size() - offset)
    {
        return std::nullopt;
    }
    return DecodeFixedFileVersion(resource.subspan(offset, fixedSize));
}

}  // namespace

std::optional<RuntimeVersion> DecodeFixedFileVersion(
    std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() != sizeof(VS_FIXEDFILEINFO))
    {
        return std::nullopt;
    }

    VS_FIXEDFILEINFO info{};
    std::memcpy(&info, bytes.data(), sizeof(info));
    if (info.dwSignature != VS_FFI_SIGNATURE || info.dwStrucVersion != VS_FFI_STRUCVERSION)
    {
        return std::nullopt;
    }

    return RuntimeVersion{
        HIWORD(info.dwFileVersionMS), LOWORD(info.dwFileVersionMS),
        HIWORD(info.dwFileVersionLS), LOWORD(info.dwFileVersionLS)};
}

std::optional<RuntimeVersion> ReadCurrentExecutableVersion() noexcept
{
    try
    {
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return std::nullopt;
        }

        DWORD ignored{};
        const auto size = GetFileVersionInfoSizeW(path.data(), &ignored);
        if (size == 0)
        {
            return std::nullopt;
        }
        std::vector<std::byte> resource(size);
        if (!GetFileVersionInfoW(path.data(), 0, size, resource.data()))
        {
            return std::nullopt;
        }
        return ReadVersionResource(resource);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

}  // namespace fo4hdr
