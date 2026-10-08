// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include "FO4Isotope/game/ImageSpaceSemantic.hpp"
#include "FO4Isotope/output/NativeLutObservation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fo4hdr {
inline constexpr std::size_t kImageSpaceActiveDataFieldCount = 12;

[[nodiscard]] bool DecodeImageSpaceActiveData(
    std::span<const std::byte> source,
    const std::array<std::size_t, kImageSpaceActiveDataFieldCount>& offsets,
    ImageSpaceActiveData& destination) noexcept;

struct ImageSpaceEffectListState
{
    std::uint16_t capacity{};
    std::uint16_t size{};
    std::uint16_t freeIndex{};
    std::uintptr_t storage{};
};

[[nodiscard]] bool ValidateImageSpaceEffectListPrefix(
    const ImageSpaceEffectListState& state,
    std::uint16_t requiredPrefixCount) noexcept;

}  // namespace fo4hdr

namespace RE { class ImageSpaceManager; class ImageSpaceLUTData; class ImageSpaceBaseData; }
namespace fo4hdr { struct NativeLutObservation; }
namespace fo4hdr::game {
[[nodiscard]] bool InitializeImageSpaceObserver(bool requested,
    const void* trampolineInterface, std::uint32_t pluginHandle) noexcept;
void AttachImageSpaceObservation(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, NativeLutObservation& result) noexcept;
}

namespace fo4hdr::game::imagespace_detail {
using Status = ImageSpaceObservationStatus;
struct WeatherGradeSlotColors
{
    std::array<std::uint8_t, 3> sunlight{};
    std::array<std::uint8_t, 3> glare{};
    std::array<std::array<std::uint8_t, 3>, 6> diffuse{};
};

[[nodiscard]] bool DecodeWeatherGradeFeatures(
    const std::array<WeatherGradeSlotColors, 8>& colors,
    const std::array<std::uint8_t, 8>& chronologicalSlots,
    std::size_t slot, WeatherGradeFeatures& result) noexcept;
void AccumulateWeatherGradeReference(WeatherGradeAggregate& aggregate,
    const WeatherGradeFeatures* features) noexcept;
void FinalizeWeatherGradeAggregate(WeatherGradeAggregate& aggregate) noexcept;

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
    std::array<WeatherGradeAggregate, 4> weatherGrades{};
    std::uint32_t validSlots{};
    std::uint32_t weatherGradeValidSlots{};
    std::uint64_t generation{};
    std::uint64_t revision{};
    std::uint64_t observationRevision{};
    std::uintptr_t manager{};
    Status status{Status::waiting};

    bool Matches(const LutValues& current, std::uint64_t epoch) const noexcept
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

    Token Begin() noexcept;
    void Invalidate() noexcept;

    bool Finish(Token& token, const Provenance& candidate) noexcept;

    bool TryRead(Provenance& result) noexcept;

    bool IsCurrent(std::uint64_t revision) const noexcept;

private:
    bool CanPublish(const Token& token) const noexcept;
    std::atomic<std::uint32_t> active_{};
    std::atomic<std::uint64_t> revision_{};
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    Provenance payload_{};
};
}

namespace fo4hdr::game {
void BuildImageSpaceCatalog() noexcept;
[[nodiscard]] bool ImageSpaceCatalogReady() noexcept;
[[nodiscard]] bool LookupImageSpaceIdentity(
    const RE::ImageSpaceBaseData* data, ImageSpaceIdentity& result,
    std::array<char, 260>& lutPath, WeatherGradeAggregate& weatherGrade) noexcept;
}  // namespace fo4hdr::game

namespace fo4hdr { class GameSnapshotStore; }
namespace fo4hdr::game::observation {
void ObserveImageSpace(GameSnapshotStore& store, std::string_view trigger);
void ReportImageSpaceSemanticProfile();
void ReportImageSpaceDetails(const GameHdrSnapshot& snapshot);
}  // namespace fo4hdr::game::observation
