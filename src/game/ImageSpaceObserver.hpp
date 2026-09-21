// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace RE { class ImageSpaceManager; class ImageSpaceBaseData; class ImageSpaceLUTData; }

namespace fo4hdr::game {

// The serialized collector builds the immutable process-lifetime catalog once.
[[nodiscard]] bool BuildImageSpaceCatalog() noexcept;
[[nodiscard]] bool ObserveImageSpace(const RE::ImageSpaceManager* manager,
    std::uint64_t resourceGeneration, ImageSpaceSnapshot& destination) noexcept;

[[nodiscard]] bool ImageSpaceCatalogReady() noexcept;
[[nodiscard]] bool LookupImageSpaceIdentity(const RE::ImageSpaceBaseData* data,
    ImageSpaceIdentity& result, std::array<char, 260>& lutPath) noexcept;
void AttachImageSpaceObservation(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, NativeLutObservation& result) noexcept;

struct ImageSpaceTargetFacts
{
    std::uint64_t id{};
    std::uintptr_t rva{};
    std::array<std::uint8_t, 32> bytes{};
    std::uint8_t byteCount{};
};

struct ImageSpaceCallFacts
{
    std::uint64_t callerId{};
    std::uintptr_t callerRva{}, rva{};
    std::array<std::uint8_t, 5> bytes{};
    std::uint8_t targetIndex{};
};

struct ImageSpaceRuntimeFacts
{
    std::array<ImageSpaceTargetFacts, 4> targets{};
    std::array<ImageSpaceCallFacts, 11> calls{};
    std::uint64_t globalLutId{}, globalBaseId{};
    std::uintptr_t globalLutRva{}, globalBaseRva{};
};

struct ImageSpaceInstallation
{
    ImageSpaceRuntimeFacts facts{};
    const void* trampolineInterface{};
    std::uint32_t pluginHandle{UINT32_MAX};
};

[[nodiscard]] bool InitializeImageSpaceObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept;

}  // namespace fo4hdr::game

namespace fo4hdr::game::imagespace_detail {
using Status = ImageSpaceObservationStatus;

class TryPublicationGuard final
{
public:
    explicit TryPublicationGuard(std::atomic_flag& flag) noexcept :
        flag_(flag), acquired_(!flag.test_and_set(std::memory_order_acquire)) {}
    ~TryPublicationGuard()
    {
        if (acquired_) flag_.clear(std::memory_order_release);
    }
    TryPublicationGuard(const TryPublicationGuard&) = delete;
    TryPublicationGuard& operator=(const TryPublicationGuard&) = delete;
    [[nodiscard]] bool OwnsLock() const noexcept { return acquired_; }
private:
    std::atomic_flag& flag_;
    bool acquired_{};
};

struct LutValues
{
    std::array<std::array<char, 260>, 4> paths{};
    std::array<float, 4> weights{};
    bool operator==(const LutValues&) const = default;
};

struct Provenance
{
    LutValues values{};
    std::array<ImageSpaceIdentity, 4> identities{};
    std::uint32_t validSlots{};
    std::uint64_t generation{}, revision{}, observationRevision{};
    std::uintptr_t manager{};
    Status status{Status::waiting};

    [[nodiscard]] bool Matches(const LutValues& current, std::uint64_t epoch) const noexcept
    { return status == Status::matched && generation == epoch && values == current; }
};

class Publication final
{
public:
    struct Token
    {
        std::uint64_t revision{};
        bool exclusive{};
    };

    [[nodiscard]] Token Begin() noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool Finish(Token& token, const Provenance& candidate) noexcept;
    [[nodiscard]] bool TryRead(Provenance& result) noexcept;
    [[nodiscard]] bool IsCurrent(std::uint64_t revision) const noexcept;

private:
    [[nodiscard]] bool CanPublish(const Token& token) const noexcept;
    std::atomic<std::uint32_t> active_{};
    std::atomic<std::uint64_t> revision_{};
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    Provenance payload_{};
};

}  // namespace fo4hdr::game::imagespace_detail
