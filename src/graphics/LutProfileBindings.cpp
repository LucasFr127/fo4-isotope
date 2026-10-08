// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
#include "LutProfileBindings.hpp"
#include "passes/NativeReferenceGradePass.hpp"
#include "resources/ReplacementShaders.hpp"
#include "RendererLifetime.hpp"
#include "LiveSettings.hpp"
#include "PresentFrame.hpp"

#include <algorithm>
#include <cstring>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

namespace {
WeatherGradeResponseCache g_weatherGradeResponses;
}

void RefreshWeatherGradeResponses() noexcept
{
    (void)g_weatherGradeResponses.Refresh(ReadWeatherGradeCatalog(),
        g_activeSettings.weatherGradeResponse);
}

std::array<LutSnapshot, 4> g_lutInspectorLuts{};
std::array<ImageSpaceIdentity, 4> g_lutInspectorImageSpaces{};
std::uint32_t g_lutInspectorImageSpaceSlots{};
const char* g_lutInspectorImageSpaceStatus{"Waiting for a qualified ImageSpace blend."};
std::uint64_t g_lutInspectorFrame{};
std::uint32_t g_lutInspectorValidatedSlots{};
const char* g_lutInspectorStatus{"Waiting for a color grading draw."};
std::array<std::uint32_t, 4> g_lutInspectorReferencingContexts{};
std::array<ResolvedLutGrade, 4> g_lutInspectorResolved{};
std::uint32_t g_lutInspectorAutomaticSlots{};
std::array<const char*, 4> g_lutInspectorFallbackReasons{};
bool g_lutInspectorGradingBypassed{};

const char* NativeLutObserverMessage() noexcept
{
    constexpr std::array messages{
        std::pair{NativeLutObserverStatus::ready, "Waiting for a color grading draw."},
        std::pair{NativeLutObserverStatus::unsupportedRuntime, "LUT identity observation is unavailable for this runtime."},
        std::pair{NativeLutObserverStatus::invalidTarget, "Native LUT boundary could not be validated; global controls are used."},
        std::pair{NativeLutObserverStatus::conflictingHook, "Native LUT boundary is owned by another hook; global controls are used."}};
    const auto status = GetNativeLutObserverStatus();
    const auto found = std::find_if(messages.begin(), messages.end(),
        [status](const auto& entry) { return entry.first == status; });
    return found == messages.end() ? "Native LUT observer unavailable; global controls are used." : found->second;

}

void PublishImageSpaceStatus(const NativeLutObservation& native) noexcept
{
    constexpr std::array messages{
        std::pair{ImageSpaceObservationStatus::matched, "ImageSpace contributors matched the native LUT transfer."},
        std::pair{ImageSpaceObservationStatus::catalogUnavailable, "ImageSpace record catalog unavailable; LUT/global controls are used."},
        std::pair{ImageSpaceObservationStatus::differentThread, "ImageSpace producer and draw threads are not qualified; LUT/global controls are used."},
        std::pair{ImageSpaceObservationStatus::overrideActive, "A native ImageSpace override is active; LUT/global controls are used."},
        std::pair{ImageSpaceObservationStatus::unqualifiedSource, "Room, direct or unidentified ImageSpace blend; LUT/global controls are used."},
        std::pair{ImageSpaceObservationStatus::updating, "ImageSpace observation is updating; LUT/global controls are used."},
        std::pair{ImageSpaceObservationStatus::stale, "ImageSpace transfer is stale or changed; LUT/global controls are used."},
        std::pair{ImageSpaceObservationStatus::unavailable, "ImageSpace observation unavailable; LUT/global controls are used."}};
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
    return IsQualifiedNativeReferenceLook(view);
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
    const ResolvedLutGrade global{g_activeSettings.vanillaGradeLuminancePercent,
        g_activeSettings.vanillaGradeColorPercent};
    const auto& aggregate = native.weatherGrades[i];
    g_lutInspectorReferencingContexts[i] = aggregate.referenceCount;
    if ((g_lutInspectorAutomaticSlots & (1U << i)) != 0)
    {
        ResolvedLutGrade resolved{};
        if (g_weatherGradeResponses.TryResolve(aggregate, resolved)) return resolved;
        g_lutInspectorAutomaticSlots &= ~(1U << i);
        g_lutInspectorFallbackReasons[i] = "Weather response cache does not match this source";
        return global;
    }

    g_lutInspectorFallbackReasons[i] =
        (g_lutInspectorImageSpaceSlots & (1U << i)) == 0 ?
            "Weather source is not qualified" :
        aggregate.referenceCount == 0 ? "No weather references" :
            "Weather aggregate is unavailable or incomplete";
    return global;
}

bool ResolveLutProfileConstants(const NativeLutObservation& native, OperatorConstants& constants) noexcept
{
    g_lutInspectorAutomaticSlots = EligibleWeatherGradeSlots(native, g_lutInspectorValidatedSlots);
    bool customized{};
    for (std::size_t i = 0; i < native.luts.size(); ++i)
    {
        const auto resolved = ResolveLutProfileSlot(native, i);
        g_lutInspectorResolved[i] = resolved;
        constants.lutProfileLuminanceRemoval[i] =
            static_cast<float>(1.0 - resolved.luminancePercent / 100.0);
        constants.lutProfileColorRemoval[i] =
            static_cast<float>(1.0 - resolved.colorPercent / 100.0);
        customized = customized ||
            constants.lutProfileLuminanceRemoval[i] != constants.selectiveColorPlaneA[3] ||
            constants.lutProfileColorRemoval[i] != constants.selectiveColorPlaneB[3];
    }
    return customized;
}

void UseGlobalWeatherResponse(const char* reason) noexcept
{
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorResolved.fill({g_activeSettings.vanillaGradeLuminancePercent,
        g_activeSettings.vanillaGradeColorPercent});
    g_lutInspectorFallbackReasons.fill(reason);
}

void MarkWeatherResponseBypassed() noexcept
{
    g_lutInspectorGradingBypassed = true;
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorStatus = "Production binding failed; grading was bypassed for this draw.";
    g_lutInspectorFallbackReasons.fill("Production grading was bypassed");
}

bool UpdateLutProfileBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
    const OperatorConstants& constants) noexcept
{
    g_replacements.lutProfileKeyReady = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        g_lutInspectorStatus = "LUT identities verified; response upload failed. Global controls are used.";
        UseGlobalWeatherResponse("Response upload failed");
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
        g_lutInspectorStatus = "LUT identities verified; response buffer unavailable. Global controls are used.";
        UseGlobalWeatherResponse("Response buffer is unavailable");
        return fallback;
    }
    constants.lutProfilesActive = 1;
    if (!g_replacements.lutProfileKeyReady ||
        std::memcmp(&constants, &g_replacements.lutProfileKey, sizeof(constants)) != 0)
    {
        if (!UpdateLutProfileBuffer(context, buffer, constants)) return fallback;
    }
    g_lutInspectorStatus = "Fixed weather responses follow the native blend; other slots use global controls.";
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
    g_lutInspectorResolved.fill({g_activeSettings.vanillaGradeLuminancePercent,
        g_activeSettings.vanillaGradeColorPercent});
    g_lutInspectorAutomaticSlots = 0;
    g_lutInspectorGradingBypassed = false;
    g_lutInspectorFallbackReasons.fill("Weather source is not qualified");
    g_lutInspectorImageSpaceStatus = "Waiting for a qualified ImageSpace blend.";
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
    g_lutInspectorStatus = "LUT identities do not match this draw; global controls are used.";
    const bool mismatched = !ValidateLutProfileViews(context, native);
    if (mismatched || g_lutInspectorValidatedSlots == 0) {
        g_lutInspectorImageSpaceStatus = "ImageSpace identities are not verified for this draw; global controls are used.";
        return false;
    }
    g_lutInspectorImageSpaceSlots = EligibleImageSpaceSlots(native, g_lutInspectorValidatedSlots);
    if (native.imageSpaceValidSlots && !g_lutInspectorImageSpaceSlots)
        g_lutInspectorImageSpaceStatus = "ImageSpace provenance was revoked; LUT/global controls are used.";

    g_lutInspectorStatus = "Verified weather slots can use automatic responses; other slots use global controls.";
    return true;
}

bool PrepareLutProfileInspector(ID3D11DeviceContext* context, const DrawObservation& record,
    const NativeLutObservation& native) noexcept
{
    if (!IsLutProfileDrawCurrent(context, record, native)) return false;

    if (!ValidateLutProfileIdentity(context, native)) return false;
    if (!g_activeSettings.weatherGradeResponse.enabled)
    {
        g_lutInspectorStatus = "Automatic weather response is disabled; global controls are used.";
        g_lutInspectorFallbackReasons.fill("Automatic response is disabled");
        return false;
    }
    if (!g_weatherGradeResponses.Matches(ReadWeatherGradeCatalog(),
            g_activeSettings.weatherGradeResponse))
    {
        g_lutInspectorStatus = "Weather response cache is pending; global controls are used.";
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

ID3D11Buffer* PrepareLutProfileDraw(
    ID3D11DeviceContext* context, const DrawObservation& record,
    ID3D11Buffer* fallback) noexcept
{
    if (GetCurrentThreadId() != g_observedPresentThread.load(std::memory_order_acquire) ||
        !g_liveSettingsThreadCompatible.load(std::memory_order_acquire))
        return fallback;
    ResetLutProfileInspector();
    const auto& native = CurrentNativeLutObservation();
    if (!PrepareLutProfileInspector(context, record, native)) return fallback;

    auto constants = g_replacements.configurationKey;
    const bool customized = ResolveLutProfileConstants(native, constants);
    if (!customized) return fallback;
    auto* prepared = UploadLutProfileConstants(context, constants, fallback);
    if (prepared == fallback) return fallback;
    if (!IsWeatherResponseDrawCurrent(context, record, native))
    {
        g_lutInspectorStatus = "Weather provenance changed; global controls are used.";
        UseGlobalWeatherResponse("Weather provenance changed");
        return fallback;
    }
    return prepared;

}

}  // namespace fo4hdr
