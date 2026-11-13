// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/game/GameHdrSnapshot.hpp"
#include "FO4Isotope/output/WeatherGradeResponse.hpp"

#include <array>
#include <cstdint>

namespace fo4hdr {

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
    bool operator==(const ImageSpaceIdentity&) const = default;
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
    std::array<WeatherGradeAggregate, 4> weatherGrades{};
    std::uint32_t imageSpaceValidSlots{};
    std::uint32_t weatherGradeValidSlots{};
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

// DeveloperMode diagnostic counts of the manager LUT copies the hooks saw; nothing
// is counted until counting is enabled.
struct ImageSpaceCopyCounts
{
    std::uint64_t copies{};
    // Copies that reinstalled the published LUTs and so opened no transfer.
    std::uint64_t reinstalled{};
    // Copies made on a different thread from the copy before.
    std::uint64_t threadChanges{};
};
void EnableImageSpaceCopyCounts(bool enabled) noexcept;
void CountImageSpaceCopy(bool reinstalled, std::uint32_t threadId) noexcept;
[[nodiscard]] ImageSpaceCopyCounts ReadImageSpaceCopyCounts() noexcept;
[[nodiscard]] std::uint32_t EligibleImageSpaceSlots(
    const NativeLutObservation& observation, std::uint32_t validatedLuts) noexcept;
[[nodiscard]] std::uint32_t EligibleWeatherGradeSlots(
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
