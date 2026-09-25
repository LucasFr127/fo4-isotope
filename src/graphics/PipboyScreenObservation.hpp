// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstddef>
#include <cstdint>

namespace fo4hdr {

enum class PipboyScreenShader : std::uint32_t { unknown, producer, consumer };

// Capture-derived diagnostic identities, not additions to the replacement set.
[[nodiscard]] constexpr PipboyScreenShader IdentifyPipboyScreenShader(
    std::uint32_t crc32, std::size_t size) noexcept
{
    if (crc32 == 0xB7FC78BBu && size == 1840) return PipboyScreenShader::producer;
    if (crc32 == 0x807CFBC4u && size == 3768) return PipboyScreenShader::consumer;
    return PipboyScreenShader::unknown;
}

// Render-thread counters describe observed resource lineage, not pixel contents
// or permission to change a draw. COM ownership remains in the renderer.
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

    // Keep the completed report visible after lifecycle retirement.
    constexpr void Stop() noexcept { requested = false; }

    [[nodiscard]] constexpr bool Matches(
        std::uint64_t resource, std::uint64_t context) const noexcept
    {
        return requested && resource == generation && context == contextGeneration;
    }

    constexpr void Producer(std::uint64_t frame) noexcept
    {
        if (!requested || frame == 0) return;
        ++producerDraws;
        lastProducerFrame = frame;
    }

    constexpr void Consumer(std::uint64_t frame, bool sameResource,
        bool producerAvailable) noexcept
    {
        if (!requested || frame == 0) return;
        ++consumerDraws;
        lastConsumerFrame = frame;
        if (!producerAvailable || lastProducerFrame == 0)
        {
            ++pendingConsumers;
        }
        else if (sameResource && lastProducerFrame <= frame)
        {
            ++matchedConsumers;
            lastMatchedFrame = frame;
            lastProducerAge = frame - lastProducerFrame;
        }
        else
        {
            ++unmatchedConsumers;
        }
    }
};

}  // namespace fo4hdr
