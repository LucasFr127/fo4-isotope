// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <type_traits>

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

struct NativeLutObservation
{
    std::array<LutSnapshot, 4> luts{};
    std::array<std::uint64_t, 4> expectedShaderViews{};
    std::uint64_t resourceGeneration{}, serial{};
    std::uint32_t threadId{}, validSlots{};
};

// Only the current synchronous callback can read this scope. Nested or invalid
// entries mask the outer value; copied identities never retain COM references.
class NativeLutScope
{
public:
    explicit NativeLutScope(const NativeLutObservation& observation) noexcept;
    ~NativeLutScope();
    NativeLutScope(const NativeLutScope&) = delete;
    NativeLutScope& operator=(const NativeLutScope&) = delete;

private:
    const NativeLutObservation* previous_{};
    NativeLutObservation observation_{};
};

[[nodiscard]] bool ReadCurrentNativeLutObservation(
    std::uint64_t resourceGeneration, NativeLutObservation& result) noexcept;
// Resource/load invalidation exchanges only a scalar, never engine payload.
void ResetNativeLutObservation(std::uint64_t resourceGeneration) noexcept;
[[nodiscard]] std::uint64_t NativeLutResourceGeneration() noexcept;

static_assert(std::is_trivially_copyable_v<ImageSpaceSnapshot>);
static_assert(std::is_trivially_copyable_v<NativeLutObservation>);

}  // namespace fo4hdr
