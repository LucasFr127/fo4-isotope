// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageSpaceObserver.hpp"
#include "DirectCallPatch.hpp"
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
#include "Fallout4HDR/config/HdrSettings.hpp"
#include <RE/Fallout.h>
#include "GameObservationMemory.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <span>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaximumImageSpaces{8192};
constexpr std::size_t kMaximumEditorIdEntries{1U << 20U};
constexpr std::string_view kNativeTexturePrefix{"Textures\\"};
static_assert(offsetof(RE::TESImageSpace, lutTexture) +
    offsetof(RE::TESTexture, textureName) == 0x80);

struct CatalogEntry
{
    std::uintptr_t dataIdentity{};
    std::uintptr_t formIdentity{};
    fo4hdr::ImageSpaceIdentity identity{};
    std::array<char, 260> lutPath{};
    bool ambiguousEditorId{};
};

using Catalog = std::vector<CatalogEntry>;

std::atomic<DWORD> g_ownerThread{};
std::unique_ptr<const Catalog> g_catalog;
std::uint64_t g_catalogGeneration{};
std::atomic_flag g_catalogFlag = ATOMIC_FLAG_INIT;

bool FormatKey(std::string_view basename, std::uint32_t localFormId,
    fo4hdr::ImageSpaceIdentity& identity)
{
    constexpr std::size_t suffixLength{7};
    std::array<char, 259 + 1> raw{};
    std::copy(basename.begin(), basename.end(), raw.begin());
    raw[basename.size()] = '|';
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 6; ++index)
    {
        const auto shift = static_cast<unsigned>((5 - index) * 4);
        raw[basename.size() + 1 + index] = hex[(localFormId >> shift) & 0xFU];
    }
    return fo4hdr::NormalizeImageSpaceGradeKey(
        {raw.data(), basename.size() + suffixLength}, identity.key);
}

std::string_view PluginBasename(const RE::TESFile& origin)
{
    const auto filename = origin.GetFilename();
    const auto separator = filename.find_last_of("/\\");
    return separator == std::string_view::npos ? filename :
        filename.substr(separator + 1);
}

bool ValidPluginBasename(std::string_view basename)
{
    constexpr std::size_t suffixLength{7};
    return !basename.empty() && basename.find_first_of("<>:\"/\\|?*") == std::string_view::npos &&
        basename.size() + suffixLength <= 259;
}

bool BuildKey(RE::TESImageSpace& form, const RE::TESFile& origin,
    fo4hdr::ImageSpaceIdentity& identity)
{
    const auto basename = PluginBasename(origin);
    if (!ValidPluginBasename(basename)) return false;
    const auto localFormId = form.GetLocalFormID();
    if (localFormId == 0 || localFormId > 0x00FFFFFFU) return false;

    return FormatKey(basename, localFormId, identity);
}

bool CopyEditorId(const RE::BSFixedString& source,
    std::array<char, 128>& result) noexcept
{
    std::array<char, 128> bytes{};
    if (!fo4hdr::game::observation::ReadBoundedText(source.c_str(), bytes)) return false;
    const std::string_view value(bytes.data());
    std::array<char, 259 + 1> validated{};
    if (!fo4hdr::NormalizeLutGradePath(value, validated)) return false;
    std::copy(value.begin(), value.end(), result.begin());
    return true;
}

void MergeEditorId(CatalogEntry& entry, const std::array<char, 128>& editorId)
{
    if (entry.identity.editorId.front() == '\0')
        entry.identity.editorId = editorId;
    else if (entry.identity.editorId != editorId)
    {
        entry.identity.editorId = {};
        entry.ambiguousEditorId = true;
    }
}

void EnrichEditorId(Catalog& catalog, const RE::TESForm* form, const RE::BSFixedString& name)
{
    const auto formIdentity = reinterpret_cast<std::uintptr_t>(form);
    const auto entry = std::lower_bound(catalog.begin(), catalog.end(), formIdentity,
        [](const CatalogEntry& candidate, std::uintptr_t target) {
            return candidate.formIdentity < target;
        });
    if (entry == catalog.end()) return;
    if (entry->formIdentity != formIdentity ||
        entry->ambiguousEditorId) return;
    std::array<char, 128> editorId{};
    if (!CopyEditorId(name, editorId)) return;
    MergeEditorId(*entry, editorId);
}

void ReadEditorMap(Catalog& catalog)
{
    auto [map, lock] = RE::TESForm::GetAllFormsByEditorID();
    if (map == nullptr) return;
    RE::BSAutoReadLock guard(lock.get());
    if (map->size() > kMaximumEditorIdEntries) return;
    for (const auto& mapped : *map) EnrichEditorId(catalog, mapped.second, mapped.first);
}

void EnrichEditorIds(Catalog& catalog) noexcept
{
    try
    {
        std::sort(catalog.begin(), catalog.end(),
            [](const CatalogEntry& left, const CatalogEntry& right) {
                return left.formIdentity < right.formIdentity;
            });
        ReadEditorMap(catalog);
    }
    catch (...)
    {
        for (auto& entry : catalog) entry.identity.editorId = {};
    }
}

void CopyLutPath(std::string_view textureNameSource, CatalogEntry& entry)
{
    const std::string_view textureName = textureNameSource;
    if (!textureName.empty() &&
        kNativeTexturePrefix.size() + textureName.size() <=
            259)
    {
        std::array<char, 260> rawPath{};
        auto output = std::copy(kNativeTexturePrefix.begin(),
            kNativeTexturePrefix.end(), rawPath.begin());
        std::copy(textureName.begin(), textureName.end(), output);
        (void)fo4hdr::NormalizeLutGradePath(
            {rawPath.data(), kNativeTexturePrefix.size() + textureName.size()},
            entry.lutPath);
    }
}

const RE::TESFile* OriginFile(RE::TESImageSpace* form)
{
    if (form == nullptr ||
        !fo4hdr::game::observation::IsReadableMemory(form, sizeof(RE::TESImageSpace))) return nullptr;
    const auto formId = form->GetFormID();
    if ((formId >> 24U) == 0xFFU) return nullptr;
    return form->GetFile(0);
}

void AddCatalogEntry(Catalog& catalog, RE::TESImageSpace* form)
{
    const auto* origin = OriginFile(form);
    if (origin == nullptr) return;

    CatalogEntry entry{
        .dataIdentity = reinterpret_cast<std::uintptr_t>(
            std::addressof(form->data)),
        .formIdentity = reinterpret_cast<std::uintptr_t>(form)};
    if (entry.dataIdentity == 0 || !BuildKey(*form, *origin, entry.identity))
        return;
    entry.identity.formId = form->GetFormID();
    CopyLutPath(form->lutTexture.textureName, entry);
    catalog.push_back(entry);
}

template<class Projection>
bool SortUnique(Catalog& catalog, Projection project)
{
    std::ranges::sort(catalog, {}, project);
    return std::ranges::adjacent_find(catalog, {}, project) == catalog.end();
}
bool CollectCatalog(Catalog& catalog)
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler == nullptr) return false;
    auto& forms = dataHandler->GetFormArray<RE::TESImageSpace>();
    if (forms.size() > kMaximumImageSpaces) return false;

    catalog.reserve(forms.size());
    for (auto* form : forms)
    {
        AddCatalogEntry(catalog, form);
    }
    return true;
}

std::unique_ptr<const Catalog> BuildCatalog()
{
    auto catalog = std::make_unique<Catalog>();
    if (!CollectCatalog(*catalog)) return {};
    if (!SortUnique(*catalog, [](const CatalogEntry& entry) { return entry.identity.key; })) return {};

    EnrichEditorIds(*catalog);
    if (!SortUnique(*catalog, &CatalogEntry::dataIdentity)) return {};

    for (auto& entry : *catalog)
    {
        entry.formIdentity = 0;
        entry.ambiguousEditorId = false;
    }

    // Engine traversal stays on the collector; readers copy only immutable values.
    return catalog;
}

}  // namespace

namespace fo4hdr::game {

bool BuildImageSpaceCatalog(std::uint64_t resourceGeneration) noexcept
{
    const auto thread = ::GetCurrentThreadId();
    DWORD expected{};
    if (!g_ownerThread.compare_exchange_strong(expected, thread) && expected != thread) return false;
    {
        imagespace_detail::TryPublicationGuard guard(g_catalogFlag);
        if (!guard.OwnsLock()) return false;
        g_catalogGeneration = UINT64_MAX;
        // Readers cannot copy the retired catalog after its revision is revoked.
        (void)InvalidateImageSpaceObservation();
    }
    try
    {
        auto catalog = BuildCatalog();
        if (!catalog) return false;
        imagespace_detail::TryPublicationGuard guard(g_catalogFlag);
        if (!guard.OwnsLock()) return false;
        // Only the cold collector destroys the old vector, while readers refuse.
        g_catalog = std::move(catalog);
        (void)InvalidateImageSpaceObservation();
        g_catalogGeneration = resourceGeneration;
        return true;
    }
    catch (...) { return false; }
}

bool ImageSpaceCatalogReady() noexcept
{
    imagespace_detail::TryPublicationGuard guard(g_catalogFlag);
    return guard.OwnsLock() && g_catalog != nullptr &&
        g_catalogGeneration == NativeLutResourceGeneration();
}

bool LookupImageSpaceIdentity(
    const RE::ImageSpaceBaseData* data, ImageSpaceIdentity& result,
    std::array<char, 260>& lutPath) noexcept
{
    result = {};
    lutPath = {};
    if (data == nullptr) return false;
    imagespace_detail::TryPublicationGuard guard(g_catalogFlag);
    if (!guard.OwnsLock() || g_catalogGeneration != NativeLutResourceGeneration()) return false;
    const auto* catalog = g_catalog.get();
    if (catalog == nullptr) return false;
    const auto identity = reinterpret_cast<std::uintptr_t>(data);
    const auto entry = std::lower_bound(catalog->begin(), catalog->end(), identity,
        [](const CatalogEntry& candidate, std::uintptr_t target) {
            return candidate.dataIdentity < target;
        });
    if (entry == catalog->end() || entry->dataIdentity != identity) return false;
    result = entry->identity;
    lutPath = entry->lutPath;
    return true;
}


namespace {
struct ManagerHeader
{
    std::uintptr_t base{}, overrideData{};

    bool Read(std::span<const std::byte> bytes) noexcept
    {
        std::uint8_t ready{};
        if (!observation::ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, isReady), ready) ||
            ready != 1) return false;
        return observation::ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, baseData), base) &&
            observation::ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, overRideBaseData), overrideData);
    }
};

bool CurrentCatalog(std::uint64_t resourceGeneration) noexcept
{
    if (::GetCurrentThreadId() != g_ownerThread.load()) return false;
    return resourceGeneration == NativeLutResourceGeneration() && ImageSpaceCatalogReady();
}

bool ReadActiveData(std::span<const std::byte> bytes, ImageSpaceActiveData& result) noexcept
{
    constexpr auto data = offsetof(RE::ImageSpaceManager, currentEOFData) +
        offsetof(RE::ImageSpaceData, baseData);
    constexpr auto hdr = data + offsetof(RE::ImageSpaceBaseData, hdrData);
    constexpr auto cinematic = data + offsetof(RE::ImageSpaceBaseData, cinematicData);
    constexpr std::array<std::size_t, 12> offsets{hdr, hdr + 4, hdr + 8, hdr + 12,
        hdr + 16, hdr + 20, hdr + 24, hdr + 28, hdr + 32, cinematic, cinematic + 4, cinematic + 8};
    return DecodeImageSpaceActiveData(bytes, offsets, result);
}
}  // namespace

bool ObserveImageSpace(const RE::ImageSpaceManager* manager,
    std::uint64_t resourceGeneration, ImageSpaceSnapshot& destination) noexcept
{
    destination = {};
    if (!CurrentCatalog(resourceGeneration) || manager == nullptr) return false;
    std::array<std::byte, sizeof(RE::ImageSpaceManager)> bytes{};
    if (!observation::GuardedReadValue(manager, bytes)) return false;
    ManagerHeader header{};
    if (!header.Read(bytes)) return false;
    ImageSpaceSnapshot candidate{};
    if (!ReadActiveData(bytes, candidate.active)) return false;
    candidate.baseMatched = LookupImageSpaceIdentity(
        reinterpret_cast<const RE::ImageSpaceBaseData*>(header.base), candidate.base, candidate.baseLutPath);
    candidate.managerIdentity = reinterpret_cast<std::uintptr_t>(manager);
    candidate.baseDataIdentity = header.base;
    candidate.overrideActive = header.overrideData != 0;
    candidate.threadId = ::GetCurrentThreadId();
    candidate.resourceGeneration = resourceGeneration;
    candidate.ready = true;
    destination = candidate;
    return true;
}

}  // namespace fo4hdr::game

namespace {
namespace provenance = fo4hdr::game::imagespace_detail;
namespace patch = fo4hdr::game::direct_call_patch;
using namespace fo4hdr::game::observation;
using Weather = void (*)(RE::Sky*);
using Accumulate = void (*)(RE::ImageSpaceBaseData*, const RE::ImageSpaceBaseData*, float);
using Blend = void (*)(RE::ImageSpaceBaseData*, const RE::ImageSpaceBaseData*,
    const RE::ImageSpaceBaseData*, float);
using SetLut = void (*)(RE::ImageSpaceManager*, const RE::ImageSpaceLUTData*);
struct TrampolineInterface
{
    std::uint32_t version{}, padding{};
    void* (__cdecl* branch)(std::uint32_t, std::size_t){};
    void* (__cdecl* local)(std::uint32_t, std::size_t){};
};
static_assert(sizeof(TrampolineInterface) == 0x18);
static_assert(offsetof(RE::ImageSpaceManager, lutData) == 0x130);
static_assert(offsetof(RE::ImageSpaceManager, overrideLUTData) == 0x1A0);
static_assert(offsetof(RE::ImageSpaceLUTData, weight) == 0x20);
std::atomic<bool> g_attempted{}, g_installed{};
std::atomic<Weather> g_weather{};
std::atomic<Accumulate> g_accumulate{};
std::atomic<Blend> g_blend{};
std::atomic<SetLut> g_setLut{};
provenance::Publication g_weatherPublication, g_transferPublication;
RE::ImageSpaceLUTData* g_globalLut{};
RE::ImageSpaceBaseData* g_globalBase{};
thread_local provenance::Provenance g_weatherCandidate;
thread_local provenance::Publication::Token g_roomToken;
thread_local std::uint32_t g_depth{};
thread_local bool g_nested{};
thread_local std::uint32_t g_count{};

bool ReadPath(const char* text, float weight, std::array<char, 260>& result) noexcept
{
    if (!text) return weight == 0;
    std::array<char, 260> raw{};
    if (!ReadBoundedText(text, raw)) return false;
    if (!raw[0]) return weight == 0;
    return fo4hdr::NormalizeLutGradePath(raw.data(), result);
}

bool ReadValueSlot(std::span<const std::byte> bytes, std::size_t slot,
    provenance::LutValues& values) noexcept
{
    std::uintptr_t name{};
    auto& weight = values.weights[slot];
    if (!ReadSnapshotField(bytes, offsetof(RE::ImageSpaceLUTData, weight) + slot * 4, weight)) return false;
    if (!ReadSnapshotField(bytes, offsetof(RE::ImageSpaceLUTData, filename) + slot * 8, name)) return false;
    if (!std::isfinite(weight) || weight < 0) return false;
    return ReadPath(reinterpret_cast<const char*>(name), weight, values.paths[slot]);
}

bool ReadValues(const RE::ImageSpaceLUTData* data, provenance::LutValues& values) noexcept
{
    std::array<std::byte, sizeof(RE::ImageSpaceLUTData)> bytes{};
    if (!GuardedReadValue(data, bytes)) return false;
    for (std::size_t slot = 0; slot < 4; ++slot)
    {
        if (!ReadValueSlot(bytes, slot, values)) return false;
    }
    std::array<std::byte, sizeof(RE::ImageSpaceLUTData)> after{};
    return GuardedReadValue(data, after) && after == bytes;
}

const RE::ImageSpaceLUTData* InlineLut(const RE::ImageSpaceManager* manager) noexcept
{
    const auto address = reinterpret_cast<std::uintptr_t>(manager);
    return address != 0 && address <= UINTPTR_MAX - sizeof(RE::ImageSpaceManager) ?
        reinterpret_cast<const RE::ImageSpaceLUTData*>(address + offsetof(RE::ImageSpaceManager, lutData)) : nullptr;
}

provenance::Status ManagerValues(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, provenance::LutValues& values) noexcept
{
    std::array<std::byte, sizeof(RE::ImageSpaceManager)> bytes{};
    if (!GuardedReadValue(manager, bytes)) return provenance::Status::unqualifiedSource;
    std::uintptr_t overrideLut{};
    std::uint8_t ready{};
    if (!ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, overrideLUTData), overrideLut) ||
        !ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, isReady), ready))
        return provenance::Status::unqualifiedSource;
    if (overrideLut) return provenance::Status::overrideActive;
    if (ready != 1 || data != InlineLut(manager)) return provenance::Status::unqualifiedSource;
    return ReadValues(data, values) ? provenance::Status::matched : provenance::Status::unqualifiedSource;
}

void DisableObservation() noexcept
{
    g_installed.store(false, std::memory_order_release);
    fo4hdr::RevokeNativeLutObservation();
    g_weatherPublication.Invalidate();
    g_transferPublication.Invalidate();
    g_nested = true;
}

bool SingleWeatherScope() noexcept
{ return !g_nested && g_depth == 1 && g_count > 0 && g_count <= 4; }
bool WeatherComplete(const provenance::Provenance& candidate) noexcept
{
    return SingleWeatherScope() &&
        candidate.generation == fo4hdr::NativeLutObservationGeneration() &&
        candidate.status == provenance::Status::waiting && candidate.validSlots &&
        candidate.observationRevision == fo4hdr::CurrentImageSpaceObservationRevision();
}
void FinishWeather(provenance::Publication::Token& token) noexcept
{
    auto& candidate = g_weatherCandidate;
    if (WeatherComplete(candidate))
        candidate.status = provenance::Status::matched;
    else if (candidate.status == provenance::Status::waiting || g_nested)
        candidate.status = provenance::Status::unqualifiedSource;
    // Paths and weights were captured with their contributor calls. The
    // native global may already belong to another producer after unlock.
    (void)g_weatherPublication.Finish(token, candidate);
    if (g_depth) --g_depth;
    if (!g_depth) g_nested = false;
}

provenance::Publication::Token BeginWeather() noexcept
{
    auto token = g_weatherPublication.Begin();
    g_transferPublication.Invalidate();
    const auto revision = fo4hdr::InvalidateImageSpaceObservation();
    if (g_depth++ != 0) g_nested = true;
    g_weatherCandidate = {};
    g_weatherCandidate.generation = fo4hdr::NativeLutObservationGeneration();
    g_weatherCandidate.observationRevision = revision;
    g_count = 0;
    return token;
}

void WeatherHook(RE::Sky* sky) noexcept
{
    const auto next = g_weather.load(std::memory_order_acquire);
    if (!next || next == &WeatherHook) return;
    const auto observe = g_installed.load(std::memory_order_acquire);
    provenance::Publication::Token token{};
    if (observe) {
        token = BeginWeather();
    }
    try {
        next(sky);
    } catch (...) {
        DisableObservation();
    }
    if (observe) {
        FinishWeather(token);
    }
}

bool QualifiedContributor(const RE::ImageSpaceBaseData* destination, float weight) noexcept
{
    return destination == g_globalBase && g_depth == 1 && !g_nested &&
        g_count < 4 && std::isfinite(weight) && weight >= 0;
}
bool ResolveContributor(const RE::ImageSpaceBaseData* source, float weight, std::size_t slot) noexcept
{
    return fo4hdr::game::LookupImageSpaceIdentity(source,
        g_weatherCandidate.identities[slot], g_weatherCandidate.values.paths[slot]) &&
        (weight == 0 || g_weatherCandidate.values.paths[slot][0]);
}
void ObserveContributor(RE::ImageSpaceBaseData* destination,
    const RE::ImageSpaceBaseData* source, float weight) noexcept
{
    if (!QualifiedContributor(destination, weight)) {
        g_weatherCandidate.status = provenance::Status::unqualifiedSource;
        g_weatherPublication.Invalidate();
    } else {
        // Native code appends only present contributors; callsite index
        // is not the LUT slot when an earlier IMGS pointer was absent.
        const auto slot = g_count++;
        g_weatherCandidate.values.weights[slot] = weight;
        if (!fo4hdr::game::ImageSpaceCatalogReady())
            g_weatherCandidate.status = provenance::Status::catalogUnavailable;
        else if (ResolveContributor(source, weight, slot))
            g_weatherCandidate.validSlots |= 1U << slot;
        else if (weight > 0)
            g_weatherCandidate.status = provenance::Status::unqualifiedSource;
    }
}

void AccumulateHook(RE::ImageSpaceBaseData* destination,
    const RE::ImageSpaceBaseData* source, float weight) noexcept
{
    const auto next = g_accumulate.load(std::memory_order_acquire);
    if (!next || next == &AccumulateHook) return;
    try {
        if (g_installed.load(std::memory_order_acquire)) {
            ObserveContributor(destination, source, weight);
        }
        next(destination, source, weight);
    } catch (...) {
        DisableObservation();
    }
}

void RoomBlendHook(RE::ImageSpaceBaseData* destination,
    const RE::ImageSpaceBaseData* previous, const RE::ImageSpaceBaseData* current,
    float fraction) noexcept
{
    const auto next = g_blend.load(std::memory_order_acquire);
    if (!next || next == &RoomBlendHook) return;
    try {
        if (g_installed.load(std::memory_order_acquire)) {
            if (g_roomToken.entered) DisableObservation();
            else {
                // The caller writes LUT names/weights after this blend returns.
                // Keep the transaction open through its corresponding setter.
                g_roomToken = g_weatherPublication.Begin();
                g_transferPublication.Invalidate();
                (void)fo4hdr::InvalidateImageSpaceObservation();
            }
        }
        next(destination, previous, current, fraction);
    } catch (...) {
        DisableObservation();
        (void)g_weatherPublication.Finish(g_roomToken, {});
    }
}

bool TransferMatches(RE::ImageSpaceManager* manager, const provenance::Provenance& candidate) noexcept
{
    provenance::LutValues values{};
    return manager && g_weatherPublication.IsCurrent(candidate.revision) &&
        ManagerValues(manager, InlineLut(manager), values) == provenance::Status::matched &&
        candidate.Matches(values, fo4hdr::NativeLutObservationGeneration()) &&
        g_weatherPublication.IsCurrent(candidate.revision);
}

bool PrepareTransfer(const RE::ImageSpaceLUTData* source, std::uint64_t previousRevision,
    provenance::Provenance& candidate) noexcept
{
    return source == g_globalLut && g_weatherPublication.TryRead(candidate) &&
        candidate.status == provenance::Status::matched &&
        candidate.observationRevision == previousRevision &&
        candidate.generation == fo4hdr::NativeLutObservationGeneration();
}
bool TransferRevisionMatches(bool prepared, std::uint64_t revision) noexcept
{
    if (!prepared || revision == 0) return false;
    return revision == fo4hdr::CurrentImageSpaceObservationRevision();
}

void CompleteTransfer(RE::ImageSpaceManager* manager, bool prepared,
    std::uint64_t observationRevision, provenance::Provenance& candidate) noexcept
{
    if (TransferRevisionMatches(prepared, observationRevision) &&
        TransferMatches(manager, candidate)) {
        candidate.manager = reinterpret_cast<std::uintptr_t>(manager);
        candidate.observationRevision = observationRevision;
    } else if (candidate.status == provenance::Status::matched)
        candidate.status = provenance::Status::unqualifiedSource;
}

struct TransferStart
{
    provenance::Publication::Token token{};
    std::uint64_t revision{}, previousRevision{};
};

TransferStart BeginTransfer(bool observe) noexcept
{
    const auto previousRevision = fo4hdr::CurrentImageSpaceObservationRevision();
    auto token = observe ? g_transferPublication.Begin() : provenance::Publication::Token{};
    const auto revision = observe ? fo4hdr::InvalidateImageSpaceObservation() : 0;
    return {token, revision, previousRevision};
}
void SetLutHook(RE::ImageSpaceManager* manager, const RE::ImageSpaceLUTData* source) noexcept
{
    const auto next = g_setLut.load(std::memory_order_acquire);
    if (!next || next == &SetLutHook) return;
    const auto observe = g_installed.load(std::memory_order_acquire);
    auto start = BeginTransfer(observe);
    provenance::Provenance candidate{};
    candidate.status = provenance::Status::unqualifiedSource;
    try {
        const auto prepared = observe && PrepareTransfer(source, start.previousRevision, candidate);
        next(manager, source);
        if (observe) {
            CompleteTransfer(manager, prepared, start.revision, candidate);
        }
    } catch (...) {
        DisableObservation();
        candidate.status = provenance::Status::unqualifiedSource;
    }
    if (observe) (void)g_transferPublication.Finish(start.token, candidate);
}

void ForwardUnqualifiedLut(RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* source, SetLut next) noexcept
{
    const auto observe = g_installed.load(std::memory_order_acquire);
    auto token = observe ? g_transferPublication.Begin() : provenance::Publication::Token{};
    if (observe) {
        (void)fo4hdr::InvalidateImageSpaceObservation();
        g_weatherPublication.Invalidate();
    }
    try {
        next(manager, source);
    } catch (...) { DisableObservation(); }
    provenance::Provenance rejected{};
    rejected.status = provenance::Status::unqualifiedSource;
    if (observe) (void)g_transferPublication.Finish(token, rejected);
}

void RoomSetLutHook(RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* source) noexcept
{
    const auto next = g_setLut.load(std::memory_order_acquire);
    if (!next || next == &RoomSetLutHook) return;
    ForwardUnqualifiedLut(manager, source, next);
    (void)g_weatherPublication.Finish(g_roomToken, {});
}

void UnqualifiedSetLutHook(RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* source) noexcept
{
    const auto next = g_setLut.load(std::memory_order_acquire);
    if (!next || next == &UnqualifiedSetLutHook) return;
    ForwardUnqualifiedLut(manager, source, next);
}

bool CurrentTransfer(const RE::ImageSpaceManager* manager, const provenance::LutValues& values,
    const provenance::Provenance& transfer) noexcept
{
    return transfer.manager == reinterpret_cast<std::uintptr_t>(manager) &&
        transfer.Matches(values, fo4hdr::NativeLutObservationGeneration()) &&
        g_transferPublication.IsCurrent(transfer.revision) &&
        transfer.observationRevision != 0 &&
        transfer.observationRevision == fo4hdr::CurrentImageSpaceObservationRevision();
}
bool Read(const void* source, void* destination, std::size_t size) noexcept
{
    SIZE_T copied{};
    return source && ::ReadProcessMemory(::GetCurrentProcess(), source,
        destination, size, &copied) && copied == size;
}
bool ContainsBytes(std::uintptr_t base, std::size_t extent, std::uintptr_t address, std::size_t size) noexcept
{
    return address >= base && address - base < extent && size <= extent - (address - base);
}
bool Region(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& info) noexcept
{
    if (!address || !size) return false;
    if (!::VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return ContainsBytes(base, info.RegionSize, start, size);
}
bool ExecutableProtection(DWORD flags) noexcept
{
    const auto protection = flags & 0xFF;
    return protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
}

bool Executable(const void* address, std::size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!Region(address, size, info)) return false;
    return ExecutableProtection(info.Protect);
}
bool MatchesBytes(std::uintptr_t address, std::span<const std::uint8_t> expected) noexcept
{
    std::array<std::uint8_t, 32> actual{};
    return expected.size() <= actual.size() && Read(reinterpret_cast<void*>(address),
        actual.data(), expected.size()) && std::equal(expected.begin(), expected.end(), actual.begin());
}
bool VerifyWrite(std::uintptr_t address, std::span<const std::uint8_t> bytes, DWORD protection) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    return Region(reinterpret_cast<void*>(address), bytes.size(), info) &&
        info.Protect == protection && MatchesBytes(address, bytes);
}

bool WriteBytes(std::uintptr_t address, std::span<const std::uint8_t> bytes,
    DWORD protection) noexcept
{
    DWORD prior{};
    if (!::VirtualProtect(reinterpret_cast<void*>(address), bytes.size(),
        PAGE_EXECUTE_READWRITE, &prior)) return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
    DWORD ignored{};
    const auto restored = ::VirtualProtect(reinterpret_cast<void*>(address), bytes.size(),
        protection, &ignored) != FALSE;
    const auto flushed = ::FlushInstructionCache(::GetCurrentProcess(),
        reinterpret_cast<void*>(address), bytes.size()) != FALSE;
    return restored && flushed && VerifyWrite(address, bytes, protection);
}
struct PatchBackend
{
    bool Matches(std::uintptr_t address, const patch::CallBytes& bytes) const noexcept
    { return MatchesBytes(address, bytes); }
    bool Replace(const patch::Site& site, const patch::CallBytes& expected,
        const patch::CallBytes& replacement) const noexcept
    {
        return Matches(site.address, expected) &&
            WriteBytes(site.address, replacement, site.protection);
    }
};
class NativeProvenanceInstallation
{
    REX::FModule module{REX::FModule::GetExecutingModule()};
    std::uintptr_t base{module.GetBaseAddress()};
    REL::Offset2ID mappings;
    fo4hdr::game::ImageSpaceRuntimeFacts facts;
    TrampolineInterface allocator{};
    std::array<patch::Site, 11> sites{};
    const std::array<std::uintptr_t, 6> hooks{
        reinterpret_cast<std::uintptr_t>(&WeatherHook), reinterpret_cast<std::uintptr_t>(&AccumulateHook),
        reinterpret_cast<std::uintptr_t>(&RoomBlendHook), reinterpret_cast<std::uintptr_t>(&SetLutHook),
        reinterpret_cast<std::uintptr_t>(&RoomSetLutHook), reinterpret_cast<std::uintptr_t>(&UnqualifiedSetLutHook)};

    std::uintptr_t Address(std::uintptr_t rva) const noexcept
    { return rva <= UINTPTR_MAX - base ? base + rva : 0; }

    bool InSection(const char* name, std::uintptr_t address, std::size_t size) const
    {
        const auto section = module.GetSection(name);
        MEMORY_BASIC_INFORMATION info{};
        return ContainsBytes(section.GetAddress(), section.GetSize(), address, size) &&
            Region(reinterpret_cast<void*>(address), size, info) &&
            info.AllocationBase == reinterpret_cast<void*>(base) && info.Type == MEM_IMAGE;
    }
    bool Mapping(std::uint64_t id, std::uintptr_t rva) const
    {
        return id != 0 && rva != 0 && std::any_of(mappings.begin(), mappings.end(), [&](const auto& entry) {
            return entry.id == id && entry.offset == rva;
        });
    }
    bool TargetValid(const fo4hdr::game::ImageSpaceTargetFacts& target) const
    {
        if (target.byteCount == 0 || target.byteCount > target.bytes.size()) return false;
        const auto address = Address(target.rva);
        const std::span bytes(target.bytes.data(), target.byteCount);
        return Mapping(target.id, target.rva) && InSection(".text", address, bytes.size()) &&
            Executable(reinterpret_cast<void*>(address), bytes.size()) && MatchesBytes(address, bytes) &&
            std::find(hooks.begin(), hooks.end(), address) == hooks.end();
    }
    bool ValidateTargets() const
    {
        for (const auto& target : facts.targets) if (!TargetValid(target)) return false;
        return true;
    }
    bool GlobalsValid() const
    {
        return Mapping(facts.globalLutId, facts.globalLutRva) &&
            Mapping(facts.globalBaseId, facts.globalBaseRva) &&
            InSection(".data", Address(facts.globalLutRva), sizeof(RE::ImageSpaceLUTData)) &&
            InSection(".data", Address(facts.globalBaseRva), sizeof(RE::ImageSpaceBaseData));
    }
    bool ReadAllocator(const fo4hdr::game::ImageSpaceInstallation& request)
    {
        return Read(request.trampolineInterface, &allocator, sizeof(allocator)) && allocator.version == 1 &&
            request.pluginHandle != UINT32_MAX && Executable(reinterpret_cast<void*>(allocator.branch), 1);
    }
    bool CallMatches(const fo4hdr::game::ImageSpaceCallFacts& call, std::uintptr_t address) const
    {
        return call.targetIndex < facts.targets.size() && MatchesBytes(address, call.bytes) &&
            patch::CallTarget(address, call.bytes) == Address(facts.targets[call.targetIndex].rva);
    }
    bool PrepareSite(std::size_t index)
    {
        const auto& call = facts.calls[index];
        const auto address = Address(call.rva);
        MEMORY_BASIC_INFORMATION info{};
        const bool valid = Mapping(call.callerId, call.callerRva) &&
            InSection(".text", Address(call.callerRva), 1) && InSection(".text", address, 5) &&
            Executable(reinterpret_cast<void*>(address), 5) && CallMatches(call, address) &&
            Region(reinterpret_cast<void*>(address), 5, info);
        if (!valid) return false;
        sites[index] = {address, call.bytes, {}, info.Protect};
        return true;
    }
    bool PrepareSites()
    {
        for (std::size_t index = 0; index < sites.size(); ++index) if (!PrepareSite(index)) return false;
        return true;
    }
    bool PrepareRelay(std::size_t index, std::uint32_t handle)
    {
        constexpr std::array<std::size_t, 11> callbacks{0, 0, 0, 1, 1, 1, 1, 2, 3, 4, 5};
        constexpr std::array<std::uint8_t, 11> targets{0, 0, 0, 1, 1, 1, 1, 2, 3, 3, 3};
        if (facts.calls[index].targetIndex != targets[index]) return false;
        auto* relay = allocator.branch(handle, 14);
        MEMORY_BASIC_INFORMATION info{};
        if (!Executable(relay, 14) || !Region(relay, 14, info)) return false;
        if (!patch::EncodeCall(sites[index].address, reinterpret_cast<std::uintptr_t>(relay),
                sites[index].replacement)) return false;
        std::array<std::uint8_t, 14> bytes{0xFF, 0x25, 0, 0, 0, 0};
        const auto hook = hooks[callbacks[index]];
        std::memcpy(bytes.data() + 6, &hook, sizeof(hook));
        return WriteBytes(reinterpret_cast<std::uintptr_t>(relay), bytes, info.Protect);
    }
    bool PrepareRelays(std::uint32_t handle)
    {
        for (std::size_t index = 0; index < sites.size(); ++index)
            if (!PrepareRelay(index, handle)) return false;
        return true;
    }
    bool Publish()
    {
        g_globalLut = reinterpret_cast<RE::ImageSpaceLUTData*>(Address(facts.globalLutRva));
        g_globalBase = reinterpret_cast<RE::ImageSpaceBaseData*>(Address(facts.globalBaseRva));
        // Captured targets and branch relays remain resident even after partial rollback.
        g_weather.store(reinterpret_cast<Weather>(Address(facts.targets[0].rva)), std::memory_order_release);
        g_accumulate.store(reinterpret_cast<Accumulate>(Address(facts.targets[1].rva)), std::memory_order_release);
        g_blend.store(reinterpret_cast<Blend>(Address(facts.targets[2].rva)), std::memory_order_release);
        g_setLut.store(reinterpret_cast<SetLut>(Address(facts.targets[3].rva)), std::memory_order_release);
        PatchBackend backend;
        if (!patch::InstallPair(sites, backend)) return false;
        g_installed.store(true, std::memory_order_release);
        return true;
    }
    bool Prepare(const fo4hdr::game::ImageSpaceInstallation& request)
    {
        if (!ValidateTargets()) return false;
        if (!GlobalsValid()) return false;
        return ReadAllocator(request);
    }
    bool PinOwner() const noexcept
    {
        HMODULE resident{};
        return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&WeatherHook), &resident) != FALSE;
    }
    bool PrepareTransaction(std::uint32_t handle)
    {
        if (!PrepareSites()) return false;
        if (!PrepareRelays(handle)) return false;
        return ValidateTargets();
    }
public:
    explicit NativeProvenanceInstallation(const fo4hdr::game::ImageSpaceRuntimeFacts& input) : facts(input) {}

    bool Install(const fo4hdr::game::ImageSpaceInstallation& request)
    {
        if (!base || module.GetFileVersion() != REL::Version{1, 10, 163, 0}) return false;
        mappings.load_v2();
        if (!Prepare(request)) return false;
        if (!PinOwner()) return false;
        if (!PrepareTransaction(request.pluginHandle)) return false;
        return Publish();
    }
};
}  // namespace

namespace fo4hdr::game {

void AttachImageSpaceObservation(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, NativeLutObservation& result) noexcept
{
    result.imageSpaces = {};
    result.imageSpaceValidSlots = 0;
    result.imageSpaceStatus = ImageSpaceObservationStatus::unavailable;
    result.imageSpaceRevision = 0;
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (!ImageSpaceCatalogReady())
    {
        result.imageSpaceStatus = ImageSpaceObservationStatus::catalogUnavailable;
        return;
    }
    provenance::Provenance transfer{};
    if (!g_transferPublication.TryRead(transfer))
    {
        result.imageSpaceStatus = transfer.status;
        return;
    }
    result.imageSpaceStatus = transfer.status;
    if (transfer.status != provenance::Status::matched) return;
    provenance::LutValues values{};
    result.imageSpaceStatus = ManagerValues(manager, data, values);
    if (result.imageSpaceStatus != provenance::Status::matched) return;
    if (!CurrentTransfer(manager, values, transfer))
    {
        result.imageSpaceStatus = ImageSpaceObservationStatus::stale;
        return;
    }
    result.imageSpaces = transfer.identities;
    result.imageSpaceValidSlots = transfer.validSlots & result.validSlots;
    result.imageSpaceRevision = transfer.observationRevision;
}

bool InitializeImageSpaceObserver(const ImageSpaceInstallation& request) noexcept
{
    if (!request.Admitted()) return false;
    if (g_attempted.exchange(true, std::memory_order_acq_rel)) return g_installed.load(std::memory_order_acquire);
    try
    {
        NativeProvenanceInstallation installation(request.facts);
        return installation.Install(request);
    }
    catch (...) { return false; }
}

}  // namespace fo4hdr::game
