// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
#include "LutProfileBindings.hpp"
#include "passes/AffineGradePass.hpp"
#include "resources/ReplacementShaders.hpp"
#include "RendererLifetime.hpp"
#include "LiveSettings.hpp"
#include "PresentFrame.hpp"
#include "WeatherResponseDrops.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

namespace {
WeatherGradeResponseCache g_weatherGradeResponses;
WeatherGradeResponseHold g_weatherResponseHold;
WeatherResponseDrops g_weatherResponseDrops;
// Color grading draws that arrived off the Present thread and so kept the Fallback Grade.
std::atomic<std::uint32_t> g_foreignThreadLutDraws{};
std::atomic<std::uint32_t> g_heldLutDraws{};
}

void RefreshWeatherGradeResponses() noexcept
{
    // A held response was resolved under the previous settings.
    if (g_weatherGradeResponses.Refresh(ReadWeatherGradeCatalog(),
            EffectiveWeatherGradeResponse(g_activeSettings)))
        g_weatherResponseHold.Reset();
}

std::array<LutSnapshot, 4> g_lutInspectorLuts{};
std::array<ImageSpaceIdentity, 4> g_lutInspectorImageSpaces{};
std::uint32_t g_lutInspectorImageSpaceSlots{};
StatusMessage g_lutInspectorImageSpaceStatus{kImageSpaceWaitingStatus};
std::uint64_t g_lutInspectorFrame{};
std::uint32_t g_lutInspectorValidatedSlots{};
StatusMessage g_lutInspectorStatus{"Waiting for a color grading draw."};
std::array<std::uint32_t, 4> g_lutInspectorReferencingContexts{};
std::array<ResolvedLutGrade, 4> g_lutInspectorResolved{};
std::uint32_t g_lutInspectorAutomaticSlots{};
std::array<const char*, 4> g_lutInspectorFallbackReasons{};
bool g_lutInspectorGradingBypassed{};

StatusMessage NativeLutObserverMessage() noexcept
{
    constexpr auto warning = StatusSeverity::warning;
    constexpr std::array messages{
        std::pair{NativeLutObserverStatus::ready, StatusMessage{"Waiting for a color grading draw."}},
        std::pair{NativeLutObserverStatus::unsupportedRuntime,
            StatusMessage{"LUT identity observation is unavailable for this runtime.", warning}},
        std::pair{NativeLutObserverStatus::invalidTarget,
            StatusMessage{"Vanilla LUT boundary could not be validated; the Fallback Grade settings apply.", warning}},
        std::pair{NativeLutObserverStatus::conflictingHook,
            StatusMessage{"Vanilla LUT boundary is owned by another hook; the Fallback Grade settings apply.", warning}}};
    const auto status = GetNativeLutObserverStatus();
    const auto found = std::find_if(messages.begin(), messages.end(),
        [status](const auto& entry) { return entry.first == status; });
    return found == messages.end() ?
        StatusMessage{"Vanilla LUT observer unavailable; the Fallback Grade settings apply.", warning} : found->second;

}

void PublishImageSpaceStatus(const NativeLutObservation& native) noexcept
{
    // Warnings are observer failures; the others are ordinary scene states.
    constexpr auto warning = StatusSeverity::warning;
    constexpr std::array messages{
        std::pair{ImageSpaceObservationStatus::matched,
            StatusMessage{"ImageSpace contributors matched the vanilla LUT transfer."}},
        std::pair{ImageSpaceObservationStatus::catalogUnavailable,
            StatusMessage{"ImageSpace record catalog unavailable; the Fallback Grade settings apply.", warning}},
        std::pair{ImageSpaceObservationStatus::differentThread,
            StatusMessage{"ImageSpace producer and draw threads do not match; the Fallback Grade settings apply.", warning}},
        std::pair{ImageSpaceObservationStatus::overrideActive,
            StatusMessage{"A vanilla ImageSpace override is active; the Fallback Grade settings apply."}},
        std::pair{ImageSpaceObservationStatus::unqualifiedSource,
            StatusMessage{"Room, direct or unidentified ImageSpace blend; the Fallback Grade settings apply."}},
        std::pair{ImageSpaceObservationStatus::updating,
            StatusMessage{"ImageSpace observation is updating; the Fallback Grade settings apply."}},
        std::pair{ImageSpaceObservationStatus::stale,
            StatusMessage{"ImageSpace transfer is stale or changed; the Fallback Grade settings apply."}},
        std::pair{ImageSpaceObservationStatus::unavailable,
            StatusMessage{"ImageSpace observation unavailable; the Fallback Grade settings apply.", warning}}};
    for (const auto& [status, message] : messages)
        if (native.imageSpaceStatus == status) {
            g_lutInspectorImageSpaceStatus = message;
            return;
        }

}

bool MatchesNativeLutView(ID3D11ShaderResourceView* view, std::uint64_t expected) noexcept
{
    if (expected == 0) return false;
    if (expected != reinterpret_cast<std::uintptr_t>(view)) return false;
    return IsQualifiedAffineLook(view);
}

bool ValidateLutProfileViews(ID3D11DeviceContext* context, const NativeLutObservation& native) noexcept
{
    std::array<ID3D11ShaderResourceView*, 4> raw{};
    context->PSGetShaderResources(3, static_cast<UINT>(raw.size()), raw.data());
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> views{};
    for (std::size_t i = 0; i < views.size(); ++i) views[i].Attach(raw[i]);
    bool mismatched{};
    for (std::size_t i = 0; i < views.size(); ++i)
    {
        if ((native.validSlots & (1U << i)) == 0) continue;
        if (!MatchesNativeLutView(views[i].Get(), native.expectedShaderViews[i]))
            mismatched = true;
        else
            g_lutInspectorValidatedSlots |= 1U << i;
    }
    return !mismatched;
}

ResolvedLutGrade ResolveLutProfileSlot(const NativeLutObservation& native, std::size_t i) noexcept
{
    const ResolvedLutGrade fallbackGrade{g_activeSettings.fallbackGradeLuminancePercent,
        g_activeSettings.fallbackGradeColorPercent};
    const auto& aggregate = native.weatherGrades[i];
    g_lutInspectorReferencingContexts[i] = aggregate.referenceCount;
    if ((g_lutInspectorAutomaticSlots & (1U << i)) != 0)
    {
        ResolvedLutGrade resolved{};
        if (g_weatherGradeResponses.TryResolve(aggregate, resolved)) return resolved;
        g_lutInspectorAutomaticSlots &= ~(1U << i);
        g_lutInspectorFallbackReasons[i] = "Weather response cache does not match this source";
        return fallbackGrade;
    }

    g_lutInspectorFallbackReasons[i] =
        (g_lutInspectorImageSpaceSlots & (1U << i)) == 0 ?
            "Weather source is not recognized" :
        aggregate.referenceCount == 0 ? "No weather references" :
            "Weather aggregate is unavailable or incomplete";
    return fallbackGrade;
}

// Writes the per-slot removal amounts; false when every slot equals the Fallback Grade's.
bool ApplyLutProfileGrades(const WeatherGradeResponseHold::Grades& grades,
    OperatorConstants& constants) noexcept
{
    bool customized{};
    for (std::size_t i = 0; i < grades.size(); ++i)
    {
        constants.lutProfileLuminanceRemoval[i] =
            static_cast<float>(1.0 - grades[i].luminancePercent / 100.0);
        constants.lutProfileColorRemoval[i] =
            static_cast<float>(1.0 - grades[i].colorPercent / 100.0);
        customized = customized ||
            constants.lutProfileLuminanceRemoval[i] != constants.selectiveColorPlaneA[3] ||
            constants.lutProfileColorRemoval[i] != constants.selectiveColorPlaneB[3];
    }
    return customized;
}

bool ResolveLutProfileConstants(const NativeLutObservation& native, OperatorConstants& constants) noexcept
{
    g_lutInspectorAutomaticSlots = EligibleWeatherGradeSlots(native, g_lutInspectorValidatedSlots);
    for (std::size_t i = 0; i < native.luts.size(); ++i)
        g_lutInspectorResolved[i] = ResolveLutProfileSlot(native, i);
    return ApplyLutProfileGrades(g_lutInspectorResolved, constants);
}

void UseFallbackGrade(const char* reason) noexcept
{
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorResolved.fill({g_activeSettings.fallbackGradeLuminancePercent,
        g_activeSettings.fallbackGradeColorPercent});
    g_lutInspectorFallbackReasons.fill(reason);
}

void MarkWeatherResponseBypassed() noexcept
{
    g_lutInspectorGradingBypassed = true;
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorStatus = {"Production binding failed; grading was bypassed for this draw.",
        StatusSeverity::error};
    g_lutInspectorFallbackReasons.fill("Grading replacement was bypassed");
}

bool UpdateLutProfileBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
    const OperatorConstants& constants) noexcept
{
    g_replacements.lutProfileKeyReady = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        g_lutInspectorStatus = {"LUT identities verified; response upload failed. The Fallback Grade settings apply.",
            StatusSeverity::warning};
        UseFallbackGrade("Response upload failed");
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(buffer, 0);
    g_replacements.lutProfileKey = constants;
    g_replacements.lutProfileKeyReady = true;
    return true;
}

ID3D11Buffer* UploadLutProfileConstants(ID3D11DeviceContext* context, OperatorConstants constants, ID3D11Buffer* fallback) noexcept
{
    // Weight interpolation stays on the GPU and uses the draw's original b2.
    // An unsuccessful upload must never reuse another draw's profile tuple.
    auto* buffer = g_replacements.lutProfileConstants.Get();
    if (!buffer)
    {
        g_lutInspectorStatus = {"LUT identities verified; response buffer unavailable. The Fallback Grade settings apply.",
            StatusSeverity::warning};
        UseFallbackGrade("Response buffer is unavailable");
        return fallback;
    }
    constants.lutProfilesActive = 1;
    if (!g_replacements.lutProfileKeyReady ||
        std::memcmp(&constants, &g_replacements.lutProfileKey, sizeof(constants)) != 0)
    {
        if (!UpdateLutProfileBuffer(context, buffer, constants)) return fallback;
    }
    g_lutInspectorStatus = {"Fixed weather responses follow the vanilla blend; other slots use the Fallback Grade settings."};
    return buffer;

}

void ResetLutProfileInspector() noexcept
{
    g_lutInspectorFrame = g_frame;
    g_lutInspectorLuts = {};
    g_lutInspectorValidatedSlots = 0;
    g_lutInspectorImageSpaces = {};
    g_lutInspectorImageSpaceSlots = 0;
    g_lutInspectorReferencingContexts = {};
    g_lutInspectorResolved.fill({g_activeSettings.fallbackGradeLuminancePercent,
        g_activeSettings.fallbackGradeColorPercent});
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorGradingBypassed = false;
    g_lutInspectorFallbackReasons.fill("Weather source is not recognized");
    g_lutInspectorImageSpaceStatus = kImageSpaceWaitingStatus;
    g_lutInspectorStatus = NativeLutObserverMessage();
}

bool IsLutProfileDrawCurrent(ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native) noexcept
{
    if (!IsCurrentNativeLutObservation(native, GetCurrentThreadId())) return false;
    if ((native.validSlots & ~15U) != 0) return false;
    if (!record.immediate || context != g_rendererDevice.activeContext) return false;

    return true;
}

bool ValidateLutProfileIdentity(ID3D11DeviceContext* context, const NativeLutObservation& native) noexcept
{
    g_lutInspectorLuts = native.luts;
    g_lutInspectorImageSpaces = native.imageSpaces;
    PublishImageSpaceStatus(native);
    g_lutInspectorStatus = {"LUT identities do not match this draw; the Fallback Grade settings apply.",
        StatusSeverity::warning};
    const bool mismatched = !ValidateLutProfileViews(context, native);
    if (mismatched || g_lutInspectorValidatedSlots == 0) {
        g_lutInspectorImageSpaceStatus = {"ImageSpace identities are not verified for this draw; the Fallback Grade settings apply."};
        return false;
    }
    g_lutInspectorImageSpaceSlots = EligibleImageSpaceSlots(native, g_lutInspectorValidatedSlots);
    if (native.imageSpaceValidSlots && !g_lutInspectorImageSpaceSlots)
        g_lutInspectorImageSpaceStatus = {"ImageSpace provenance was revoked; the Fallback Grade settings apply."};

    g_lutInspectorStatus = {"Verified weather slots can use automatic responses; other slots use the Fallback Grade settings."};
    return true;
}

bool PrepareLutProfileInspector(ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native) noexcept
{
    if (!IsLutProfileDrawCurrent(context, record, native)) return false;

    if (!ValidateLutProfileIdentity(context, native)) return false;
    const auto response = EffectiveWeatherGradeResponse(g_activeSettings);
    if (!response.enabled)
    {
        g_lutInspectorStatus = {"Automatic weather response is disabled; the Fallback Grade settings apply."};
        g_lutInspectorFallbackReasons.fill("Automatic response is disabled");
        return false;
    }
    if (!g_weatherGradeResponses.Matches(ReadWeatherGradeCatalog(), response))
    {
        g_lutInspectorStatus = {"Weather response cache is pending; the Fallback Grade settings apply."};
        g_lutInspectorFallbackReasons.fill("Weather response cache is pending");
        return false;
    }

    return true;
}

bool IsWeatherResponseDrawCurrent(ID3D11DeviceContext* context,
    const DrawObservation& record, const NativeLutObservation& native) noexcept
{
    return IsLutProfileDrawCurrent(context, record, native) &&
        g_lutInspectorImageSpaceSlots == EligibleImageSpaceSlots(native, g_lutInspectorValidatedSlots) &&
        g_lutInspectorAutomaticSlots == EligibleWeatherGradeSlots(native, g_lutInspectorValidatedSlots);
}

const char* UnverifiedLutReason(const NativeLutObservation& native) noexcept
{
    if (native.serial == 0) return "No vanilla LUT observation for this draw";
    return g_lutInspectorValidatedSlots == 0 ? g_lutInspectorStatus.text :
        "A weighted LUT slot has no verified texture";
}

const char* SlotDropReason(const NativeLutObservation& native, std::size_t i) noexcept
{
    const auto slot = 1U << i;
    if ((g_lutInspectorValidatedSlots & slot) == 0) return UnverifiedLutReason(native);
    if ((g_lutInspectorImageSpaceSlots & slot) == 0) return g_lutInspectorImageSpaceStatus.text;
    return g_lutInspectorFallbackReasons[i];
}

// Names why a weighted slot of this draw does not use the response; null when all do.
const char* WeatherResponseDropReason(const NativeLutObservation& native) noexcept
{
    bool weighted{};
    for (std::size_t i = 0; i < native.luts.size(); ++i)
    {
        // A slot this faint cannot show a swap to the Fallback Grade.
        if (!(native.luts[i].weight > 0.05F)) continue;
        weighted = true;
        if ((g_lutInspectorAutomaticSlots & (1U << i)) == 0) return SlotDropReason(native, i);
    }
    return weighted ? nullptr : UnverifiedLutReason(native);
}

void ObserveWeatherResponseDrop(bool enabled, const char* reason) noexcept
{
    if (!g_configuration.developerMode) return;
    if (enabled)
        g_weatherResponseDrops.Observe(g_frame, reason);
    else
        g_weatherResponseDrops.Forget();
}

// The game can set its LUTs on another thread while this pass draws, which fails
// a frame's checks without changing the scene. Such a draw keeps the response of
// the last draw that passed, for the few frames the hold allows.
ID3D11Buffer* HoldWeatherResponse(ID3D11DeviceContext* context, ID3D11Buffer* fallback) noexcept
{
    WeatherGradeResponseHold::Grades held{};
    if (!g_weatherResponseHold.TryHold(g_frame, held)) return fallback;
    auto constants = g_replacements.configurationKey;
    if (!ApplyLutProfileGrades(held, constants)) return fallback;
    auto* prepared = UploadLutProfileConstants(context, constants, fallback);
    if (prepared == fallback) return fallback;
    g_lutInspectorResolved = held;
    g_lutInspectorStatus = {
        "Weather response held from a recent frame; this draw's sources are unverified."};
    g_heldLutDraws.fetch_add(1, std::memory_order_relaxed);
    return prepared;
}

ID3D11Buffer* ResolveLutProfileDraw(
    ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native, ID3D11Buffer* fallback) noexcept
{
    if (!PrepareLutProfileInspector(context, record, native)) return fallback;

    auto constants = g_replacements.configurationKey;
    const bool customized = ResolveLutProfileConstants(native, constants);
    if (!customized) return fallback;
    auto* prepared = UploadLutProfileConstants(context, constants, fallback);
    if (prepared == fallback) return fallback;
    if (!IsWeatherResponseDrawCurrent(context, record, native))
    {
        g_lutInspectorStatus = {"Weather provenance changed; the Fallback Grade settings apply."};
        UseFallbackGrade("Weather provenance changed");
        return fallback;
    }
    return prepared;

}

ID3D11Buffer* PrepareLutProfileDraw(
    ID3D11DeviceContext* context, const DrawObservation& record,
    ID3D11Buffer* fallback) noexcept
{
    if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire) ||
        !g_liveSettingsThreadCompatible.load(std::memory_order_acquire))
    {
        // The inspector and drop record belong to the Present thread.
        g_foreignThreadLutDraws.fetch_add(1, std::memory_order_relaxed);
        return fallback;
    }
    ResetLutProfileInspector();
    const auto& native = CurrentNativeLutObservation();
    auto* prepared = ResolveLutProfileDraw(context, record, native, fallback);
    const bool enabled = EffectiveWeatherGradeResponse(g_activeSettings).enabled;
    const auto* dropped = WeatherResponseDropReason(native);
    ObserveWeatherResponseDrop(enabled, dropped);
    if (!enabled) return prepared;
    if (dropped != nullptr) return HoldWeatherResponse(context, fallback);
    g_weatherResponseHold.Store(g_frame, g_lutInspectorResolved);
    return prepared;
}

void ReportWeatherResponseDrops(GraphicsDiagnosticSink sink)
{
    if (!g_configuration.developerMode || !sink) return;
    sink(std::format(
        "weather_response foreign_thread_draws={} held_draws={} unlisted_drops={} brief_frames={}",
        g_foreignThreadLutDraws.load(std::memory_order_relaxed),
        g_heldLutDraws.load(std::memory_order_relaxed), g_weatherResponseDrops.Unlisted(),
        WeatherResponseDrops::kBriefFrames));
    g_weatherResponseDrops.ForEach([sink](const WeatherResponseDrops::Entry& entry) {
        sink(std::format(
            "weather_response_drop drops={} brief={} longest_brief_frames={} reason=\"{}\"",
            entry.drops, entry.brief, entry.longestBriefFrames, entry.reason));
    });
}

}  // namespace fo4hdr
