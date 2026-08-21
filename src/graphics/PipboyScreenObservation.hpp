// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace fo4hdr {

enum class PipboyScreenShader : std::uint32_t { unknown, producer, consumer };

// Diagnostic candidates only; these identifiers never authorize replacement.
[[nodiscard]] constexpr PipboyScreenShader IdentifyPipboyScreenShader(
    std::uint32_t crc32, std::size_t size) noexcept
{
    if (crc32 == 0xB7FC78BBu && size == 1840) return PipboyScreenShader::producer;
    if (crc32 == 0x807CFBC4u && size == 3768) return PipboyScreenShader::consumer;
    return PipboyScreenShader::unknown;
}

[[nodiscard]] inline PipboyScreenShader MatchPipboyScreenBytecode(
    std::span<const std::byte> bytecode, std::uint32_t crc32) noexcept
{
    const auto role = IdentifyPipboyScreenShader(crc32, bytecode.size());
    if (role == PipboyScreenShader::unknown) return role;
    constexpr std::array magic{std::byte{0x44}, std::byte{0x58}, std::byte{0x42}, std::byte{0x43}};
    if (!std::equal(magic.begin(), magic.end(), bytecode.begin())) return PipboyScreenShader::unknown;
    constexpr std::array<std::uint32_t, 4> producer{0x28465710, 0x966a0df5, 0xd2c8c2d7, 0x56e8e66d};
    constexpr std::array<std::uint32_t, 4> consumer{0x2ff863b6, 0xf45e6855, 0x9304fad4, 0xeb237c12};
    const auto& hash = role == PipboyScreenShader::producer ? producer : consumer;
    return std::memcmp(bytecode.data() + magic.size(), hash.data(), sizeof(hash)) == 0
        ? role : PipboyScreenShader::unknown;
}

// Submission lineage only: other writes, draw coverage and pixel contents are
// unobserved. A retained resource match cannot authorize a color transformation.
struct PipboyScreenObservation
{
    bool requested{};
    std::uint64_t generation{};
    std::uint64_t contextGeneration{};
    std::uint64_t producerDraws{};
    std::uint64_t consumerDraws{};
    std::uint64_t pendingConsumers{};
    std::uint64_t matchedConsumers{};
    std::uint64_t unmatchedConsumers{};
    std::uint64_t rejectedDraws{};
    std::uint64_t lastProducerFrame{};
    std::uint64_t lastConsumerFrame{};
    std::uint64_t lastMatchedFrame{};
    std::uint64_t lastProducerAge{};

    constexpr void Begin(std::uint64_t resource, std::uint64_t context) noexcept
    {
        *this = {};
        requested = resource != 0 && context != 0;
        generation = resource;
        contextGeneration = context;
    }

    constexpr void Stop() noexcept { requested = false; }

    [[nodiscard]] constexpr bool Matches(std::uint64_t resource, std::uint64_t context) const noexcept
    {
        return requested && resource == generation && context == contextGeneration;
    }

    constexpr void Producer(std::uint64_t frame) noexcept
    {
        if (!requested || frame == 0) return;
        ++producerDraws;
        lastProducerFrame = frame;
    }

    constexpr void Consumer(std::uint64_t frame, bool sameResource, bool producerAvailable) noexcept
    {
        if (!requested || frame == 0) return;
        ++consumerDraws;
        lastConsumerFrame = frame;
        if (!producerAvailable || lastProducerFrame == 0) ++pendingConsumers;
        else if (sameResource && lastProducerFrame <= frame)
        {
            ++matchedConsumers;
            lastMatchedFrame = frame;
            lastProducerAge = frame - lastProducerFrame;
        }
        else ++unmatchedConsumers;
    }
};

}  // namespace fo4hdr
