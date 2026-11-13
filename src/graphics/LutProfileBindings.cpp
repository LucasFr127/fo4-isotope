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
#include <string_view>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

namespace {
WeatherGradeResponseCache g_weatherGradeResponses;
WeatherGradeResponseHold g_weatherResponseHold;
WeatherResponseDrops g_weatherResponseDrops;
// The same drops after the hold decision: what reached the screen.
WeatherResponseDrops g_shownWeatherResponseDrops;
// Color grading draws that arrived off the Present thread and so kept the Fallback Grade.
std::atomic<std::uint32_t> g_foreignThreadLutDraws{};
std::atomic<std::uint32_t> g_heldLutDraws{};
// Identities of the views this draw bound to the four LUT slots; zero until read.
WeatherGradeResponseHold::Views g_lutDrawViews{};
// DeveloperMode: the shown-drop reason of the draw the hold stored, null when it showed the response.
const char* g_heldShownReason{};

// How a color grading draw's weather checks ended.
enum class LutDrawCheck : std::uint8_t
{
    // No draw can use the response now: wrong context, no observer, response off or pending.
    unavailable,
    // The checks gave no answer for this draw, so it may keep a held response.
    unanswered,
    // Every slot resolved to its response or to a source the response does not cover.
    verified
};

struct LutProfileDraw
{
    ID3D11Buffer* constants{};
    LutDrawCheck check{};
};
}

void ResetWeatherResponseHold() noexcept
{
    g_weatherResponseHold.Reset();
    g_heldShownReason = nullptr;
}

void RefreshWeatherGradeResponses() noexcept
{
    // A held response was resolved under the previous settings.
    if (g_weatherGradeResponses.Refresh(ReadWeatherGradeCatalog(),
            EffectiveWeatherGradeResponse(g_activeSettings)))
        ResetWeatherResponseHold();
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
bool g_lutInspectorHeld{};

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

// Reads the views bound to the four LUT slots and records their identities for the hold.
std::array<ComPtr<ID3D11ShaderResourceView>, 4> CaptureLutDrawViews(ID3D11DeviceContext* context) noexcept
{
    std::array<ID3D11ShaderResourceView*, 4> raw{};
    context->PSGetShaderResources(3, static_cast<UINT>(raw.size()), raw.data());
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> views{};
    for (std::size_t i = 0; i < views.size(); ++i)
    {
        views[i].Attach(raw[i]);
        g_lutDrawViews[i] = reinterpret_cast<std::uintptr_t>(raw[i]);
    }
    return views;
}

bool ValidateLutProfileViews(ID3D11DeviceContext* context, const NativeLutObservation& native) noexcept
{
    const auto views = CaptureLutDrawViews(context);
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
    g_lutDrawViews = {};
    g_lutInspectorLuts = {};
    g_lutInspectorValidatedSlots = 0;
    g_lutInspectorImageSpaces = {};
    g_lutInspectorImageSpaceSlots = 0;
    g_lutInspectorReferencingContexts = {};
    g_lutInspectorResolved.fill({g_activeSettings.fallbackGradeLuminancePercent,
        g_activeSettings.fallbackGradeColorPercent});
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorGradingBypassed = false;
    g_lutInspectorHeld = false;
    g_lutInspectorFallbackReasons.fill("Weather source is not recognized");
    g_lutInspectorImageSpaceStatus = kImageSpaceWaitingStatus;
    g_lutInspectorStatus = NativeLutObserverMessage();
}

bool IsLutProfileContext(ID3D11DeviceContext* context, const DrawObservation& record) noexcept
{
    return record.immediate && context == g_rendererDevice.activeContext;
}

bool IsLutProfileDrawCurrent(ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native) noexcept
{
    if (!IsCurrentNativeLutObservation(native, GetCurrentThreadId())) return false;
    if ((native.validSlots & ~15U) != 0) return false;
    return IsLutProfileContext(context, record);
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

// False, with the reason published, while no draw can use the response.
bool ReportResponseReady() noexcept
{
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

LutDrawCheck CheckLutProfileDraw(ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native) noexcept
{
    if (!IsLutProfileContext(context, record) ||
        GetNativeLutObserverStatus() != NativeLutObserverStatus::ready)
        return LutDrawCheck::unavailable;
    // A draw without a current observation can still keep a held response.
    const bool identified = IsLutProfileDrawCurrent(context, record, native) &&
        ValidateLutProfileIdentity(context, native);
    if (!ReportResponseReady()) return LutDrawCheck::unavailable;
    return identified ? LutDrawCheck::verified : LutDrawCheck::unanswered;
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
    if (!IsCurrentNativeLutObservation(native, GetCurrentThreadId()))
        return "No vanilla LUT observation for this draw";
    // A fixed text: the inspector status is replaced when a held draw is uploaded.
    return g_lutInspectorValidatedSlots == 0 ? "LUT identities do not match this draw" :
        "A weighted LUT slot has no verified texture";
}

const char* SlotDropReason(const NativeLutObservation& native, std::size_t i) noexcept
{
    const auto slot = 1U << i;
    if ((g_lutInspectorValidatedSlots & slot) == 0) return UnverifiedLutReason(native);
    if ((g_lutInspectorImageSpaceSlots & slot) == 0) return g_lutInspectorImageSpaceStatus.text;
    return g_lutInspectorFallbackReasons[i];
}

// A fainter slot cannot show a swap to the Fallback Grade.
bool IsWeightedLutSlot(const NativeLutObservation& native, std::size_t i) noexcept
{
    return native.luts[i].weight > 0.05F;
}

// Names why a weighted slot of this draw does not use the response; null when all do.
const char* WeatherResponseDropReason(const NativeLutObservation& native) noexcept
{
    bool weighted{};
    for (std::size_t i = 0; i < native.luts.size(); ++i)
    {
        if (!IsWeightedLutSlot(native, i)) continue;
        weighted = true;
        if ((g_lutInspectorAutomaticSlots & (1U << i)) == 0) return SlotDropReason(native, i);
    }
    return weighted ? nullptr : UnverifiedLutReason(native);
}

void ObserveWeatherResponseDrop(WeatherResponseDrops& drops, bool enabled,
    const char* reason) noexcept
{
    if (!g_configuration.developerMode) return;
    if (enabled)
        drops.Observe(g_frame, reason);
    else
        drops.Forget();
}

// True when this LUT view is bound in a slot that drew with the response.
bool IsDrawnWithResponse(std::uint64_t view) noexcept
{
    if (view == 0) return false;
    for (std::size_t slot = 0; slot < g_lutDrawViews.size(); ++slot)
        if (g_lutDrawViews[slot] == view && (g_lutInspectorAutomaticSlots & (1U << slot)) != 0)
            return true;
    return false;
}

// A carried hold can bind a weighted LUT in another slot, so each weighted LUT of
// the observation is found by its view.
bool HeldDrawShowsResponse(const NativeLutObservation& native) noexcept
{
    for (std::size_t i = 0; i < native.luts.size(); ++i)
    {
        if (!IsWeightedLutSlot(native, i)) continue;
        const auto view = native.expectedShaderViews[i] != 0 ?
            native.expectedShaderViews[i] : g_lutDrawViews[i];
        if (!IsDrawnWithResponse(view)) return false;
    }
    return true;
}

// Without weights, any bound LUT that a carry left without the response may show.
bool CarryLeftBoundLut() noexcept
{
    for (std::size_t slot = 0; slot < g_lutDrawViews.size(); ++slot)
        if (g_lutDrawViews[slot] != 0 && (g_lutInspectorAutomaticSlots & (1U << slot)) == 0)
            return true;
    return false;
}

// Names why the drawn result lacks the response; null when it has it.
const char* ShownWeatherResponseDropReason(const NativeLutObservation& native, bool held,
    const char* reason) noexcept
{
    if (!held) return reason;
    const auto* missing = reason ? reason : "A weighted LUT has no held weather response";
    if (g_lutInspectorAutomaticSlots == 0) return missing;
    // A failed render-entry read still has a serial, so the serial alone does not tell.
    // Without weights, a same-view hold shows what the stored draw showed.
    if (!IsCurrentNativeLutObservation(native, GetCurrentThreadId()))
    {
        if (!g_weatherResponseHold.Carried()) return g_heldShownReason;
        return CarryLeftBoundLut() ? missing : nullptr;
    }
    return HeldDrawShowsResponse(native) ? nullptr : missing;
}

bool IsUnsettledImageSpace(ImageSpaceObservationStatus status) noexcept
{
    using enum ImageSpaceObservationStatus;
    return status == updating || status == stale || status == waiting;
}

bool HasUnverifiedWeightedSlot(const NativeLutObservation& native) noexcept
{
    for (std::size_t i = 0; i < native.luts.size(); ++i)
        if (IsWeightedLutSlot(native, i) && (g_lutInspectorValidatedSlots & (1U << i)) == 0)
            return true;
    return false;
}

// The weather checks give no answer while the game is changing its LUT state, or
// when a weighted slot's texture cannot be tied to the observation.
bool IsWeatherSourceUnanswered(const NativeLutObservation& native) noexcept
{
    if (IsUnsettledImageSpace(native.imageSpaceStatus)) return true;
    // A source the response does not cover is an answer for every slot.
    if (native.imageSpaceStatus != ImageSpaceObservationStatus::matched) return false;
    // Matched when the pass began and revoked since.
    if (native.imageSpaceValidSlots != 0 &&
        EligibleImageSpaceSlots(native, g_lutInspectorValidatedSlots) == 0) return true;
    return HasUnverifiedWeightedSlot(native);
}

// The slots this draw resolved from ImageSpace provenance that is still current,
// with the response or with the Fallback Grade; zero when that provenance moved.
std::uint32_t AnsweredLutSlots(const NativeLutObservation& native) noexcept
{
    const auto answered = EligibleImageSpaceSlots(native, g_lutInspectorValidatedSlots);
    const bool current = answered == g_lutInspectorImageSpaceSlots &&
        g_lutInspectorAutomaticSlots == EligibleWeatherGradeSlots(native, g_lutInspectorValidatedSlots);
    return current ? answered : 0U;
}

// A slot this draw answered itself keeps its own grade, since it passed the
// draw-time revision check; other slots not held follow the current Fallback
// Grade settings.
void MergeHeldGrades(const NativeLutObservation& native, WeatherGradeResponseHold::Grades& held,
    std::uint32_t& automatic) noexcept
{
    const auto own = g_lutInspectorAutomaticSlots;
    const auto answered = AnsweredLutSlots(native) | own;
    for (std::size_t i = 0; i < held.size(); ++i)
    {
        const auto slot = 1U << i;
        if ((answered & slot) != 0)
            held[i] = g_lutInspectorResolved[i];
        else if ((automatic & slot) == 0)
            held[i] = {g_activeSettings.fallbackGradeLuminancePercent,
                g_activeSettings.fallbackGradeColorPercent};
    }
    automatic = (automatic & ~answered) | own;
}

// The game can set its LUTs on another thread while this pass draws, which leaves
// a draw's checks without an answer though the scene is unchanged. Such a draw
// keeps the response of the last verified draw while it binds the same four LUT
// views, and for a short time each of those LUTs keeps its response in another
// slot; otherwise it keeps its own result.
ID3D11Buffer* HoldWeatherResponse(ID3D11DeviceContext* context, ID3D11Buffer* own,
    ID3D11Buffer* fallback, bool& applied) noexcept
{
    // A draw without a current observation has not read its views yet.
    if (g_lutDrawViews == WeatherGradeResponseHold::Views{}) (void)CaptureLutDrawViews(context);
    WeatherGradeResponseHold::Grades held{};
    std::uint32_t automatic{};
    if (!g_weatherResponseHold.TryHold(g_frame, g_lutDrawViews, held, automatic)) return own;
    applied = true;
    MergeHeldGrades(CurrentNativeLutObservation(), held, automatic);
    g_lutInspectorResolved = held;
    g_lutInspectorAutomaticSlots = automatic;
    auto constants = g_replacements.configurationKey;
    if (!ApplyLutProfileGrades(held, constants)) return fallback;
    auto* prepared = UploadLutProfileConstants(context, constants, fallback);
    if (prepared == fallback) return fallback;
    g_lutInspectorHeld = true;
    g_lutInspectorStatus = {"Weather response held from the last verified draw's LUTs."};
    if (g_configuration.developerMode) g_heldLutDraws.fetch_add(1, std::memory_order_relaxed);
    return prepared;
}

// DeveloperMode only: records why this draw lacks the response, before the hold.
const char* RecordWeatherResponseDrop(const NativeLutObservation& native) noexcept
{
    if (!g_configuration.developerMode) return nullptr;
    const auto* reason = WeatherResponseDropReason(native);
    ObserveWeatherResponseDrop(g_weatherResponseDrops,
        EffectiveWeatherGradeResponse(g_activeSettings).enabled, reason);
    return reason;
}

// DeveloperMode only: records what reached the screen after the hold.
void RecordShownWeatherResponseDrop(const NativeLutObservation& native, bool held,
    const char* reason) noexcept
{
    if (!g_configuration.developerMode) return;
    ObserveWeatherResponseDrop(g_shownWeatherResponseDrops,
        EffectiveWeatherGradeResponse(g_activeSettings).enabled,
        ShownWeatherResponseDropReason(native, held, reason));
}

void StoreWeatherResponse(const char* reason) noexcept
{
    g_weatherResponseHold.Store(g_frame, g_lutDrawViews, g_lutInspectorResolved,
        g_lutInspectorAutomaticSlots);
    g_heldShownReason = reason;
}

LutProfileDraw ResolveLutProfileDraw(
    ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native, ID3D11Buffer* fallback) noexcept
{
    const auto check = CheckLutProfileDraw(context, record, native);
    if (check != LutDrawCheck::verified) return {fallback, check};

    auto constants = g_replacements.configurationKey;
    const bool customized = ResolveLutProfileConstants(native, constants);
    const auto resolved = IsWeatherSourceUnanswered(native) ?
        LutDrawCheck::unanswered : LutDrawCheck::verified;
    if (!customized) return {fallback, resolved};
    auto* prepared = UploadLutProfileConstants(context, constants, fallback);
    if (prepared == fallback) return {fallback, resolved};
    if (!IsWeatherResponseDrawCurrent(context, record, native))
    {
        g_lutInspectorStatus = {"Weather provenance changed; the Fallback Grade settings apply."};
        UseFallbackGrade("Weather provenance changed");
        return {fallback, LutDrawCheck::unanswered};
    }
    return {prepared, resolved};

}

ID3D11Buffer* PrepareLutProfileDraw(
    ID3D11DeviceContext* context, const DrawObservation& record,
    ID3D11Buffer* fallback) noexcept
{
    if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire) ||
        !g_liveSettingsThreadCompatible.load(std::memory_order_acquire))
    {
        // The inspector and drop record belong to the Present thread.
        if (g_configuration.developerMode)
            g_foreignThreadLutDraws.fetch_add(1, std::memory_order_relaxed);
        return fallback;
    }
    ResetLutProfileInspector();
    const auto& native = CurrentNativeLutObservation();
    const auto draw = ResolveLutProfileDraw(context, record, native, fallback);
    const auto* reason = RecordWeatherResponseDrop(native);
    auto* constants = draw.constants;
    bool held{};
    if (draw.check == LutDrawCheck::unanswered)
        constants = HoldWeatherResponse(context, draw.constants, fallback, held);
    // A verified draw always uses its own per-slot result and becomes the held one.
    else if (draw.check == LutDrawCheck::verified)
        StoreWeatherResponse(reason);
    else
        ResetWeatherResponseHold();
    RecordShownWeatherResponseDrop(native, held, reason);
    return constants;
}

void ReportWeatherResponseDrops(GraphicsDiagnosticSink sink)
{
    if (!g_configuration.developerMode || !sink) return;
    const auto copies = ReadImageSpaceCopyCounts();
    sink(std::format(
        "weather_response foreign_thread_draws={} held_draws={} unlisted_drops={} "
        "shown_unlisted_drops={} brief_frames={} lut_copies={} reinstalled_lut_copies={} "
        "lut_copy_thread_changes={}",
        g_foreignThreadLutDraws.load(std::memory_order_relaxed),
        g_heldLutDraws.load(std::memory_order_relaxed), g_weatherResponseDrops.Unlisted(),
        g_shownWeatherResponseDrops.Unlisted(), WeatherResponseDrops::kBriefFrames,
        copies.copies, copies.reinstalled, copies.threadChanges));
    const auto report = [sink](std::string_view name, const WeatherResponseDrops& drops) {
        drops.ForEach([sink, name](const WeatherResponseDrops::Entry& entry) {
            sink(std::format(
                "{} drops={} brief={} longest_brief_frames={} reason=\"{}\"",
                name, entry.drops, entry.brief, entry.longestBriefFrames, entry.reason));
        });
    };
    report("weather_response_drop", g_weatherResponseDrops);
    report("weather_response_shown_drop", g_shownWeatherResponseDrops);
}

}  // namespace fo4hdr
