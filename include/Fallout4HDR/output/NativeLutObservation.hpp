// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fo4hdr {

struct ImageSpaceActiveData
{
    float eyeAdaptSpeed{}, bloomBlurRadius{}, bloomThreshold{}, bloomScale{};
    float receiveBloomThreshold{}, white{}, sunlightScale{}, skyScale{};
    float eyeAdaptStrength{}, cinematicSaturation{}, cinematicBrightness{}, cinematicContrast{};
};

inline constexpr std::size_t kImageSpaceActiveDataFieldCount{12};
[[nodiscard]] bool DecodeImageSpaceActiveData(std::span<const std::byte> source,
    const std::array<std::size_t, kImageSpaceActiveDataFieldCount>& offsets,
    ImageSpaceActiveData& destination) noexcept;

enum class NativeLutObserverStatus : std::uint8_t
{
    unavailable,
    ready,
    unsupportedRuntime,
    invalidTarget,
    conflictingHook
};

struct ImageSpaceIdentity
{
    std::array<char, 260> key{};
    std::array<char, 128> editorId{};
    std::uint32_t formId{};
};

struct LutSnapshot
{
    // Owned UTF-8 path; ASCII case and separators are normalized.
    std::array<char, 260> utf8Path{};
    std::uint16_t pathLength{};
    float weight{};
    std::uint64_t engineTextureId{};
};

struct ImageSpaceSnapshot
{
    ImageSpaceActiveData active{};
    ImageSpaceIdentity base{};
    std::array<char, 260> baseLutPath{};
    std::uint64_t managerIdentity{}, baseDataIdentity{}, resourceGeneration{};
    std::uint32_t threadId{};
    bool ready{}, overrideActive{}, baseMatched{};
};

enum class ImageSpaceObservationStatus : std::uint8_t
{
    unavailable,
    waiting,
    matched,
    catalogUnavailable,
    differentThread,
    unqualifiedSource,
    overrideActive,
    stale,
    updating
};

struct NativeLutObservation
{
    std::array<LutSnapshot, 4> luts{};
    std::array<std::uint64_t, 4> expectedShaderViews{};
    std::array<ImageSpaceIdentity, 4> imageSpaces{};
    std::uint32_t imageSpaceValidSlots{};
    ImageSpaceObservationStatus imageSpaceStatus{ImageSpaceObservationStatus::unavailable};
    std::uint32_t validSlots{};
    std::uint32_t threadId{};
    std::uint64_t serial{};
    std::uint64_t generation{};
    std::uint64_t imageSpaceRevision{};
};

void SetNativeLutObserverStatus(NativeLutObserverStatus status) noexcept;
[[nodiscard]] NativeLutObserverStatus GetNativeLutObserverStatus() noexcept;
void ResetNativeLutObservation() noexcept;
[[nodiscard]] std::uint64_t NativeLutObservationGeneration() noexcept;
void SetImageSpaceObservationReady(bool ready) noexcept;
[[nodiscard]] bool IsImageSpaceObservationReady() noexcept;
[[nodiscard]] std::uint64_t CurrentImageSpaceObservationRevision() noexcept;
[[nodiscard]] std::uint64_t InvalidateImageSpaceObservation() noexcept;
[[nodiscard]] std::uint32_t EligibleImageSpaceSlots(
    const NativeLutObservation& observation, std::uint32_t validatedLuts) noexcept;
[[nodiscard]] const NativeLutObservation& CurrentNativeLutObservation() noexcept;
[[nodiscard]] bool IsCurrentNativeLutObservation(
    const NativeLutObservation& observation, std::uint32_t threadId) noexcept;

// Names and identities are owned values, valid only within the native draw.
// Invalid and nested entries mask the outer observation until they return.
class NativeLutScope final
{
public:
    explicit NativeLutScope(const NativeLutObservation& observation) noexcept;
    ~NativeLutScope();
    NativeLutScope(const NativeLutScope&) = delete;
    NativeLutScope& operator=(const NativeLutScope&) = delete;

private:
    NativeLutObservation previous_{};
};

}  // namespace fo4hdr
