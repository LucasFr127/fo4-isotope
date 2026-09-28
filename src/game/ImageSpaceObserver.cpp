// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ImageSpaceObserver.hpp"
#include "DirectCallPatch.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include <RE/Fallout.h>
#include <REL/Offset2ID.h>
#include <REX/FModule.h>
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
#if defined(FO4HDR_RUNTIME_1_10_163)
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#endif

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

std::atomic<bool> g_buildAttempted{};
std::atomic<const Catalog*> g_catalog{};

bool FormatKey(std::string_view basename, std::uint32_t localFormId,
    fo4hdr::ImageSpaceIdentity& identity)
{
    constexpr std::size_t suffixLength{7};
    std::array<char, fo4hdr::kMaximumLutGradePathLength + 1> raw{};
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
    return !basename.empty() && !basename.contains('|') &&
        basename.size() + suffixLength <= fo4hdr::kMaximumLutGradePathLength;
}

bool BuildKey(RE::TESImageSpace& form, const RE::TESFile& origin,
    fo4hdr::ImageSpaceIdentity& identity)
{
    const auto basename = PluginBasename(origin);
    if (!ValidPluginBasename(basename)) return false;
    const auto localFormId = form.GetLocalFormID();
    if (localFormId > 0x00FFFFFFU) return false;

    return FormatKey(basename, localFormId, identity);
}

bool CopyEditorId(const RE::BSFixedString& source,
    std::array<char, 128>& result) noexcept
{
    const std::string_view value = source;
    if (value.empty() || value.size() >= result.size()) return false;
    std::array<char, fo4hdr::kMaximumLutGradePathLength + 1> validated{};
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

void EnrichEditorIds(Catalog& catalog) noexcept
{
    try
    {
        std::sort(catalog.begin(), catalog.end(),
            [](const CatalogEntry& left, const CatalogEntry& right) {
                return left.formIdentity < right.formIdentity;
            });
        auto [map, lock] = RE::TESForm::GetAllFormsByEditorID();
        if (map == nullptr) return;
        RE::BSAutoReadLock guard(lock.get());
        if (map->size() > kMaximumEditorIdEntries) return;
        for (const auto& mapped : *map)
        {
            EnrichEditorId(catalog, mapped.second, mapped.first);
        }
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
            fo4hdr::kMaximumLutGradePathLength)
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

void AddCatalogEntry(Catalog& catalog, RE::TESImageSpace* form)
{
    if (form == nullptr) return;
    const auto formId = form->GetFormID();
    if ((formId >> 24U) == 0xFFU) return;
    const auto* origin = form->GetFile(0);
    if (origin == nullptr) return;

    CatalogEntry entry{
        .dataIdentity = reinterpret_cast<std::uintptr_t>(
            std::addressof(form->data)),
        .formIdentity = reinterpret_cast<std::uintptr_t>(form)};
    if (entry.dataIdentity == 0 || !BuildKey(*form, *origin, entry.identity))
        return;
    entry.identity.formId = formId;
    CopyLutPath(form->lutTexture.textureName, entry);
    catalog.push_back(entry);
}

template<class Projection>
bool SortUnique(Catalog& catalog, Projection project)
{
    std::ranges::sort(catalog, {}, project);
    return std::ranges::adjacent_find(catalog, {}, project) == catalog.end();
}
bool BuildCatalog()
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler == nullptr) return false;
    auto& forms = dataHandler->GetFormArray<RE::TESImageSpace>();
    if (forms.size() > kMaximumImageSpaces) return false;

    auto catalog = std::make_unique<Catalog>();
    catalog->reserve(forms.size());
    for (auto* form : forms)
    {
        AddCatalogEntry(*catalog, form);
    }

    if (!SortUnique(*catalog, [](const CatalogEntry& entry) { return entry.identity.key; })) return false;

    EnrichEditorIds(*catalog);
    if (!SortUnique(*catalog, &CatalogEntry::dataIdentity)) return false;

    for (auto& entry : *catalog)
    {
        entry.formIdentity = 0;
        entry.ambiguousEditorId = false;
    }

    // Published entries remain alive until process exit so render readers never
    // race destruction or a rebuilt catalog.
    g_catalog.store(catalog.release(), std::memory_order_release);
    return true;
}

}  // namespace

namespace fo4hdr::game {

void BuildImageSpaceCatalog() noexcept
{
    if (g_buildAttempted.exchange(true, std::memory_order_acq_rel)) return;
    try
    {
        (void)BuildCatalog();
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "FO4Isotope: ImageSpace catalog construction raised an exception; "
            "ImageSpace profiles remain unavailable.\n");
    }
}

bool ImageSpaceCatalogReady() noexcept
{
    return g_catalog.load(std::memory_order_acquire) != nullptr;
}

bool LookupImageSpaceIdentity(
    const RE::ImageSpaceBaseData* data, ImageSpaceIdentity& result,
    std::array<char, 260>& lutPath) noexcept
{
    result = {};
    lutPath = {};
    if (data == nullptr) return false;
    const auto* catalog = g_catalog.load(std::memory_order_acquire);
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

}  // namespace fo4hdr::game

namespace {
namespace provenance = fo4hdr::game::imagespace_detail;
namespace patch = fo4hdr::game::direct_call_patch;
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
    if (!text) return weight <= 0;
    std::array<char, 260> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = text[i];
        if (!raw[i]) {
            if (!i) return weight <= 0;
            return fo4hdr::NormalizeLutGradePath({raw.data(), i}, result);
        }
    }
    return false;
}

bool ReadSlot(const RE::ImageSpaceLUTData& data, std::size_t slot,
    provenance::LutValues& values) noexcept
{
    const auto weight = data.weight[slot];
    if (!std::isfinite(weight) || weight < 0) return false;
    values.weights[slot] = weight;
    return ReadPath(data.filename[slot].c_str(), weight, values.paths[slot]);
}

bool ReadValues(const RE::ImageSpaceLUTData* data, provenance::LutValues& values) noexcept
{
    __try {
        if (!data) return false;
        for (std::size_t slot = 0; slot < 4; ++slot)
            if (!ReadSlot(*data, slot, values)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
provenance::Status ManagerValues(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, provenance::LutValues& values) noexcept
{
    __try {
        if (!manager) return provenance::Status::unqualifiedSource;
        if (manager->overrideLUTData) return provenance::Status::overrideActive;
        return data == &manager->lutData && ReadValues(data, values) ?
            provenance::Status::matched : provenance::Status::unqualifiedSource;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return provenance::Status::unqualifiedSource; }
}

void DisableObservation() noexcept
{
    g_installed.store(false, std::memory_order_release);
    fo4hdr::SetImageSpaceObservationReady(false);
    (void)fo4hdr::InvalidateImageSpaceObservation();
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
        candidate.status == provenance::Status::waiting && candidate.validSlots;
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
    if (g_depth++ != 0) g_nested = true;
    g_weatherCandidate = {};
    g_weatherCandidate.generation = fo4hdr::NativeLutObservationGeneration();
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
            if (g_roomToken.revision) DisableObservation();
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
        ManagerValues(manager, &manager->lutData, values) == provenance::Status::matched &&
        candidate.Matches(values, fo4hdr::NativeLutObservationGeneration()) &&
        g_weatherPublication.IsCurrent(candidate.revision);
}

bool PrepareTransfer(const RE::ImageSpaceLUTData* source, provenance::Provenance& candidate) noexcept
{
    return source == g_globalLut && g_weatherPublication.TryRead(candidate) &&
        candidate.status == provenance::Status::matched &&
        candidate.generation == fo4hdr::NativeLutObservationGeneration();
}
void CompleteTransfer(RE::ImageSpaceManager* manager, bool prepared,
    std::uint64_t observationRevision, provenance::Provenance& candidate) noexcept
{
    if (prepared && TransferMatches(manager, candidate)) {
        candidate.manager = reinterpret_cast<std::uintptr_t>(manager);
        candidate.observationRevision = observationRevision;
    } else if (candidate.status == provenance::Status::matched)
        candidate.status = provenance::Status::unqualifiedSource;
}

struct TransferStart
{
    provenance::Publication::Token token{};
    std::uint64_t revision{};
};

TransferStart BeginTransfer(bool observe) noexcept
{
    auto token = observe ? g_transferPublication.Begin() : provenance::Publication::Token{};
    const auto revision = observe ? fo4hdr::InvalidateImageSpaceObservation() : 0;
    return {token, revision};
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
        const auto prepared = observe && PrepareTransfer(source, candidate);
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
#if defined(FO4HDR_RUNTIME_1_10_163)
using namespace fo4hdr::generated;
class ImageSpaceInstallation
{
    REX::FModule module{REX::FModule::GetExecutingModule()};
    std::uintptr_t base{module.GetBaseAddress()};
    REL::Offset2ID mappings;
    TrampolineInterface allocator{};
    std::array<patch::Site, 11> sites{};
    static constexpr std::array targetRvas{kImageSpaceWeatherRva, kImageSpaceAccumulateRva,
        kImageSpaceRoomBlendRva, kImageSpaceSetLutRva};
    static constexpr std::array targetIds{kImageSpaceWeatherId, kImageSpaceAccumulateId,
        kImageSpaceRoomBlendId, kImageSpaceSetLutId};
    static constexpr std::array<std::span<const std::uint8_t>, 4> targetBytes{
        kImageSpaceWeatherBytes, kImageSpaceAccumulateBytes,
        kImageSpaceRoomBlendBytes, kImageSpaceSetLutBytes};
    const std::array<std::uintptr_t, 6> hooks{
        reinterpret_cast<std::uintptr_t>(&WeatherHook),
        reinterpret_cast<std::uintptr_t>(&AccumulateHook),
        reinterpret_cast<std::uintptr_t>(&RoomBlendHook),
        reinterpret_cast<std::uintptr_t>(&SetLutHook),
        reinterpret_cast<std::uintptr_t>(&RoomSetLutHook),
        reinterpret_cast<std::uintptr_t>(&UnqualifiedSetLutHook)};

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
        return std::any_of(mappings.begin(), mappings.end(), [&](const auto& entry) {
            return entry.id == id && entry.offset == rva;
        });
    }
    bool TargetValid(std::size_t i) const
    {
        return Mapping(targetIds[i], targetRvas[i]) &&
            InSection(".text", base + targetRvas[i], targetBytes[i].size()) &&
            Executable(reinterpret_cast<void*>(base + targetRvas[i]), targetBytes[i].size()) &&
            MatchesBytes(base + targetRvas[i], targetBytes[i]) &&
            std::find(hooks.begin(), hooks.end(), base + targetRvas[i]) == hooks.end();
    }
    bool ValidateTargets() const
    {
        for (std::size_t i = 0; i < targetRvas.size(); ++i)
            if (!TargetValid(i)) return false;
        return true;
    }
    bool GlobalsValid() const
    {
        return Mapping(kImageSpaceGlobalLutId, kImageSpaceGlobalLutRva) &&
            Mapping(kImageSpaceGlobalBaseId, kImageSpaceGlobalBaseRva) &&
            InSection(".data", base + kImageSpaceGlobalLutRva, sizeof(RE::ImageSpaceLUTData)) &&
            InSection(".data", base + kImageSpaceGlobalBaseRva, sizeof(RE::ImageSpaceBaseData));
    }
    bool ReadAllocator(const void* allocatorInterface, std::uint32_t handle)
    {
        return Read(allocatorInterface, &allocator, sizeof(allocator)) && allocator.version == 1 &&
            handle != UINT32_MAX && Executable(reinterpret_cast<void*>(allocator.branch), 1);
    }
    bool CallMatches(std::size_t i, std::uintptr_t address) const
    {
        return MatchesBytes(address, kImageSpaceCallBytes[i]) &&
            kImageSpaceCallTargetIndices[i] < targetRvas.size() &&
            patch::CallTarget(address, kImageSpaceCallBytes[i]) ==
                base + targetRvas[kImageSpaceCallTargetIndices[i]];
    }
    bool PrepareSite(std::size_t i)
    {
        const auto address = base + kImageSpaceCallRvas[i];
        MEMORY_BASIC_INFORMATION info{};
        const bool valid = Mapping(kImageSpaceCallerIds[i], kImageSpaceCallerRvas[i]) &&
            InSection(".text", base + kImageSpaceCallerRvas[i], 1) &&
            InSection(".text", address, 5) && Executable(reinterpret_cast<void*>(address), 5) &&
            CallMatches(i, address) && Region(reinterpret_cast<void*>(address), 5, info);
        if (!valid) return false;
        sites[i] = {address, kImageSpaceCallBytes[i], {}, info.Protect};
        return true;
    }
    bool PrepareSites()
    {
        for (std::size_t i = 0; i < sites.size(); ++i)
            if (!PrepareSite(i)) return false;
        return true;
    }
    bool PrepareRelay(std::size_t i, std::uint32_t handle)
    {
        constexpr std::array<std::size_t, 11> callbacks{0, 0, 0, 1, 1, 1, 1, 2, 3, 4, 5};
        auto* relay = allocator.branch(handle, 14);
        MEMORY_BASIC_INFORMATION info{};
        if (!Executable(relay, 14) || !Region(relay, 14, info)) return false;
        if (!patch::EncodeCall(sites[i].address, reinterpret_cast<std::uintptr_t>(relay),
                sites[i].replacement)) return false;
        std::array<std::uint8_t, 14> bytes{0xFF, 0x25, 0, 0, 0, 0};
        const auto hook = hooks[callbacks[i]];
        std::memcpy(bytes.data() + 6, &hook, sizeof(hook));
        return WriteBytes(reinterpret_cast<std::uintptr_t>(relay), bytes, info.Protect);
    }
    bool PrepareRelays(std::uint32_t handle)
    {
        for (std::size_t i = 0; i < sites.size(); ++i)
            if (!PrepareRelay(i, handle)) return false;
        return true;
    }
    bool TargetsUnchanged() const
    {
        for (std::size_t i = 0; i < targetRvas.size(); ++i)
            if (!MatchesBytes(base + targetRvas[i], targetBytes[i])) return false;
        return true;
    }
    bool Publish()
    {
        g_globalLut = reinterpret_cast<RE::ImageSpaceLUTData*>(base + kImageSpaceGlobalLutRva);
        g_globalBase = reinterpret_cast<RE::ImageSpaceBaseData*>(base + kImageSpaceGlobalBaseRva);
        // All relays and forwarding targets outlive rollback, including a
        // protection failure that leaves an owned CALL reachable at shutdown.
        g_weather.store(reinterpret_cast<Weather>(base + kImageSpaceWeatherRva), std::memory_order_release);
        g_accumulate.store(reinterpret_cast<Accumulate>(base + kImageSpaceAccumulateRva), std::memory_order_release);
        g_blend.store(reinterpret_cast<Blend>(base + kImageSpaceRoomBlendRva), std::memory_order_release);
        g_setLut.store(reinterpret_cast<SetLut>(base + kImageSpaceSetLutRva), std::memory_order_release);
        PatchBackend backend;
        if (!patch::InstallPair(sites, backend)) return false;
        fo4hdr::SetImageSpaceObservationReady(true);
        g_installed.store(true, std::memory_order_release);
        return true;
    }
public:
    bool Prepare(const void* allocatorInterface, std::uint32_t handle)
    {
        if (!ValidateTargets() || !GlobalsValid()) return false;
        if (!ReadAllocator(allocatorInterface, handle)) return false;
        return PrepareSites() && PrepareRelays(handle);
    }
    bool Install(const void* allocatorInterface, std::uint32_t handle)
    {
        if (!base || module.GetFileVersion() != REL::Version{1, 10, 163, 0}) return false;
        mappings.load_v2();
        if (!Prepare(allocatorInterface, handle)) return false;
        if (!TargetsUnchanged()) return false;
        return Publish();
    }
};
#endif
}

namespace fo4hdr::game {
void AttachImageSpaceObservation(const RE::ImageSpaceManager* manager,
    const RE::ImageSpaceLUTData* data, NativeLutObservation& result) noexcept
{
    result.imageSpaces = {};
    result.imageSpaceValidSlots = 0;
    result.imageSpaceStatus = ImageSpaceObservationStatus::unavailable;
    result.imageSpaceRevision = 0;
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (!ImageSpaceCatalogReady()) {
        result.imageSpaceStatus = ImageSpaceObservationStatus::catalogUnavailable;
        return;
    }
    provenance::Provenance transfer{};
    if (!g_transferPublication.TryRead(transfer)) {
        result.imageSpaceStatus = transfer.status;
        return;
    }
    result.imageSpaceStatus = transfer.status;
    if (transfer.status != provenance::Status::matched) return;
    provenance::LutValues values{};
    result.imageSpaceStatus = ManagerValues(manager, data, values);
    if (result.imageSpaceStatus != provenance::Status::matched) return;
    if (!CurrentTransfer(manager, values, transfer)) {
        result.imageSpaceStatus = ImageSpaceObservationStatus::stale;
        return;
    }
    result.imageSpaces = transfer.identities;
    result.imageSpaceValidSlots = transfer.validSlots & result.validSlots;
    result.imageSpaceRevision = transfer.observationRevision;
}

bool InitializeImageSpaceObserver(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept
{
    if (g_attempted.exchange(true, std::memory_order_acq_rel))
        return g_installed.load(std::memory_order_acquire);
    if (!requested) return false;
#if !defined(FO4HDR_RUNTIME_1_10_163)
    return false;
#else
    try {
        ImageSpaceInstallation installation;
        return installation.Install(trampolineInterface, pluginHandle);
    } catch (...) { return false; }
#endif
}
}

namespace {
static_assert(
    sizeof(RE::ImageSpaceManager) ==
    fo4hdr::generated::kImageSpaceManagerSize);
static_assert(
    offsetof(RE::ImageSpaceManager, currentEOFData) ==
    fo4hdr::generated::kImageSpaceCurrentEofDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, effectList) ==
    fo4hdr::generated::kImageSpaceEffectListOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, baseData) ==
    fo4hdr::generated::kImageSpaceBaseDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, overRideBaseData) ==
    fo4hdr::generated::kImageSpaceOverrideBaseDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, underwaterBaseData) ==
    fo4hdr::generated::kImageSpaceUnderwaterBaseDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, consoleBaseData) ==
    fo4hdr::generated::kImageSpaceConsoleBaseDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, lutData) ==
    fo4hdr::generated::kImageSpaceLutDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, overrideLUTData) ==
    fo4hdr::generated::kImageSpaceOverrideLutDataOffset);
static_assert(
    offsetof(RE::ImageSpaceManager, isReady) ==
    fo4hdr::generated::kImageSpaceReadyOffset);
static_assert(
    static_cast<std::uint16_t>(
        RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_TOP_LEVEL_END) ==
    fo4hdr::generated::kImageSpaceTopLevelEffectCount);
static_assert(
    fo4hdr::generated::ImageSpaceEffectEnumMatches<
        RE::ImageSpaceManager::ImageSpaceEffectEnum>());
static_assert(
    fo4hdr::generated::kImageSpaceCommonLibSourceCommit ==
    fo4hdr::generated::kCommonLibCommit);
constexpr auto kDearImageSpaceActiveDataOffsets = [] {
    constexpr auto managerData =
        offsetof(RE::ImageSpaceManager, currentEOFData);
    constexpr auto baseData =
        managerData + offsetof(RE::ImageSpaceData, baseData);
    constexpr auto hdr = baseData + offsetof(RE::ImageSpaceBaseData, hdrData);
    constexpr auto cinematic =
        baseData + offsetof(RE::ImageSpaceBaseData, cinematicData);
    return std::array<std::size_t, 12>{
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptSpeed),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, bloomBlurRadius),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, bloomThreshold),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, bloomScale),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, receiveBloomThreshold),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, white),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, sunlightScale),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, skyScale),
        hdr + offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptStrength),
        cinematic +
            offsetof(RE::ImageSpaceBaseData::Cinematic, saturation),
        cinematic +
            offsetof(RE::ImageSpaceBaseData::Cinematic, brightness),
        cinematic +
            offsetof(RE::ImageSpaceBaseData::Cinematic, contrast)
    };
}();
static_assert(
    kDearImageSpaceActiveDataOffsets ==
    fo4hdr::generated::kImageSpaceActiveDataOffsets);
struct ImageSpacePrimitiveArrayView
{
    std::uintptr_t virtualTable{};
    std::uintptr_t data{};
    std::uint16_t capacity{};
    std::uint16_t freeIndex{};
    std::uint16_t size{};
    std::uint16_t growthSize{};
};
using DearImageSpacePrimitiveArray =
    RE::NiTPrimitiveArray<RE::ImageSpaceEffect*>;
static_assert(
    sizeof(ImageSpacePrimitiveArrayView) ==
    fo4hdr::generated::kImageSpacePrimitiveArraySize);
static_assert(
    sizeof(DearImageSpacePrimitiveArray) ==
    fo4hdr::generated::kImageSpacePrimitiveArraySize);
static_assert(
    alignof(ImageSpacePrimitiveArrayView) ==
    alignof(DearImageSpacePrimitiveArray));
static_assert(
    offsetof(ImageSpacePrimitiveArrayView, data) ==
    fo4hdr::generated::kImageSpacePrimitiveArrayDataOffset);
static_assert(
    offsetof(ImageSpacePrimitiveArrayView, capacity) ==
    fo4hdr::generated::kImageSpacePrimitiveArrayCapacityOffset);
static_assert(
    offsetof(ImageSpacePrimitiveArrayView, freeIndex) ==
    fo4hdr::generated::kImageSpacePrimitiveArrayFreeIndexOffset);
static_assert(
    offsetof(ImageSpacePrimitiveArrayView, size) ==
    fo4hdr::generated::kImageSpacePrimitiveArraySizeOffset);
static_assert(
    offsetof(RE::ImageSpaceEffect, isActive) ==
    fo4hdr::generated::kImageSpaceEffectStoredActiveOffset);
static_assert(
    sizeof(RE::ImageSpaceLUTData) ==
    fo4hdr::generated::kImageSpaceLutDataSize);
static_assert(
    offsetof(RE::ImageSpaceLUTData, weight) ==
    fo4hdr::generated::kImageSpaceLutWeightOffset);
static_assert(
    offsetof(RE::ImageSpaceLUTData, texture) ==
    fo4hdr::generated::kImageSpaceLutTextureOffset);
static_assert(
    std::extent_v<decltype(RE::ImageSpaceLUTData::weight)> ==
    fo4hdr::generated::kImageSpaceLutEntryCount);
static_assert(
    std::extent_v<decltype(RE::ImageSpaceLUTData::texture)> ==
    fo4hdr::generated::kImageSpaceLutEntryCount);
static_assert(
    std::tuple_size_v<decltype(
        fo4hdr::generated::kImageSpaceEffectSemantics)> ==
    std::tuple_size_v<decltype(
        fo4hdr::GameHdrSnapshot{}.imageSpaceEffects)>);
static_assert(
    std::tuple_size_v<decltype(fo4hdr::GameHdrSnapshot{}.luts)> ==
    fo4hdr::generated::kImageSpaceLutEntryCount);

}
namespace fo4hdr::game::observation {
class ImageSpaceCollector
{
    GameSnapshotStore& store;
    RE::ImageSpaceManager* manager{};
    static constexpr std::size_t kManagerSize = fo4hdr::generated::kImageSpaceManagerSize;
    static constexpr std::size_t kLutSize = fo4hdr::generated::kImageSpaceLutDataSize;
    std::array<std::byte, kManagerSize> managerBytes{};
    fo4hdr::GameHdrSnapshot observed{};
    std::uint8_t readyByte{};
    std::uintptr_t effectStorage{};
    std::uint16_t effectCapacity{};
    std::uint16_t effectFreeIndex{};
    std::uint16_t effectSize{};
    std::uintptr_t baseData{};
    std::uintptr_t overrideData{};
    std::uintptr_t underwaterData{};
    std::uintptr_t consoleData{};
    std::uintptr_t overrideLut{};
    static constexpr auto kEffectList =
        fo4hdr::generated::kImageSpaceEffectListOffset;

    bool Fail(std::string_view reason) noexcept
    {
        ReportFeatureFailure(store, fo4hdr::GameSnapshotFeature::ImageSpace, "image-space", reason);
        return false;
    }
    bool ReadManager()
    {
        manager = ValidateCommonLibObject(
            RE::ImageSpaceManager::GetSingleton(),
            sizeof(RE::ImageSpaceManager),
            "RE::ImageSpaceManager::GetSingleton");
        if (manager == nullptr || !GuardedReadValue(manager, managerBytes))
        {
            return Fail("relocation-or-object-invalid");
        }

        return true;
    }

    bool ReadEffectHeader()
    {
        using namespace fo4hdr::generated;
        return ReadSnapshotField(managerBytes, kImageSpaceReadyOffset, readyByte) &&
            ReadSnapshotField(managerBytes, kEffectList + kImageSpacePrimitiveArrayDataOffset, effectStorage) &&
            ReadSnapshotField(managerBytes, kEffectList + kImageSpacePrimitiveArrayCapacityOffset, effectCapacity) &&
            ReadSnapshotField(managerBytes, kEffectList + kImageSpacePrimitiveArrayFreeIndexOffset, effectFreeIndex) &&
            ReadSnapshotField(managerBytes, kEffectList + kImageSpacePrimitiveArraySizeOffset, effectSize);
    }

    bool ReadDataPointers()
    {
        using namespace fo4hdr::generated;
        return ReadSnapshotField(managerBytes, kImageSpaceBaseDataOffset, baseData) &&
            ReadSnapshotField(managerBytes, kImageSpaceOverrideBaseDataOffset, overrideData) &&
            ReadSnapshotField(managerBytes, kImageSpaceUnderwaterBaseDataOffset, underwaterData) &&
            ReadSnapshotField(managerBytes, kImageSpaceConsoleBaseDataOffset, consoleData) &&
            ReadSnapshotField(managerBytes, kImageSpaceOverrideLutDataOffset, overrideLut);
    }

    bool ReadFields()
    {
        if (!ReadEffectHeader() || !ReadDataPointers()) return Fail("manager-snapshot-invalid");
        return true;
    }
    bool ValidateFields()
    {
        if (readyByte > 1)
        {
            return Fail("manager-ready-byte-invalid");
        }
        if (readyByte == 0)
        {
            return Fail("manager-not-ready");
        }
        const auto effectListValid = fo4hdr::ValidateImageSpaceEffectListPrefix(
            {effectCapacity, effectSize, effectFreeIndex, effectStorage},
            fo4hdr::generated::kImageSpaceTopLevelEffectCount);
        if (!effectListValid)
        {
            REX::WARN(
                "m2 image_space_effect_list valid=false capacity={} size={} "
                "free_index={} storage_present={} required_top_level_prefix={}",
                effectCapacity,
                effectSize,
                effectFreeIndex,
                effectStorage != 0,
                fo4hdr::generated::kImageSpaceTopLevelEffectCount);
            return Fail("effect-list-prefix-invalid");
        }

        return true;
    }

    bool ReadActiveData()
    {
        observed.imageSpaceIdentity =
            reinterpret_cast<std::uintptr_t>(manager);
        observed.baseImageSpaceIdentity = baseData;
        observed.overrideImageSpaceIdentity = overrideData;
        observed.underwaterImageSpaceIdentity = underwaterData;
        observed.consoleImageSpaceIdentity = consoleData;
        observed.imageSpaceOverride = overrideData != 0;
        if (!fo4hdr::DecodeImageSpaceActiveData(
                managerBytes,
                fo4hdr::generated::kImageSpaceActiveDataOffsets,
                observed.activeImageSpace))
        {
            return Fail("active-data-invalid");
        }

        return true;
    }

    bool ReadEffect(std::size_t index)
    {
        std::uintptr_t effectIdentity{};
        const auto* effectSlot = reinterpret_cast<const std::byte*>(
            effectStorage) + index * sizeof(std::uintptr_t);
        if (!GuardedReadValue(effectSlot, effectIdentity))
        {
            return Fail("effect-list-read-failed");
        }
        auto& destination = observed.imageSpaceEffects[index];
        destination.declaredIndex = static_cast<std::uint16_t>(index);
        destination.present = effectIdentity != 0;
        destination.objectIdentity = effectIdentity;
        if (effectIdentity != 0)
        {
            std::uint8_t storedActiveByte{};
            if (!GuardedReadValue(
                    reinterpret_cast<const std::byte*>(effectIdentity) +
                        fo4hdr::generated::
                            kImageSpaceEffectStoredActiveOffset,
                    storedActiveByte) ||
                storedActiveByte > 1)
            {
                return Fail("effect-object-read-failed");
            }
            destination.active = storedActiveByte != 0;
            ++observed.imageSpaceEffectCount;
        }
        return true;
    }

    bool ReadEffects()
    {
        if (!IsReadableMemory(
                reinterpret_cast<const void*>(effectStorage),
                observed.imageSpaceEffects.size() * sizeof(std::uintptr_t),
                false))
        {
            return Fail("effect-list-storage-invalid");
        }
        for (std::size_t index = 0;
             index < observed.imageSpaceEffects.size();
             ++index)
        {
            if (!ReadEffect(index)) return false;
        }

        return true;
    }

    bool ReadLut(const std::array<std::byte, kLutSize>& lutBytes, std::size_t index)
    {
        auto& destination = observed.luts[index];
        if (!ReadSnapshotField(
                lutBytes,
                fo4hdr::generated::kImageSpaceLutWeightOffset +
                    index * sizeof(float),
                destination.weight) ||
            !ReadSnapshotField(
                lutBytes,
                fo4hdr::generated::kImageSpaceLutTextureOffset +
                    index * sizeof(std::uintptr_t),
                destination.engineTextureId))
        {
            return Fail("lut-snapshot-invalid");
        }
        if (destination.engineTextureId != 0 && destination.weight > 0.0F)
        {
            ++observed.activeLutCount;
        }
        return true;
    }

    bool ReadLutStorage(std::array<std::byte, kLutSize>& lutBytes)
    {
        if (overrideLut != 0)
        {
            if (!GuardedReadValue(
                    reinterpret_cast<const void*>(overrideLut),
                    lutBytes))
            {
                return Fail("lut-storage-invalid");
            }
        }
        else
        {
            std::memcpy(
                lutBytes.data(),
                managerBytes.data() +
                    fo4hdr::generated::kImageSpaceLutDataOffset,
                kLutSize);
        }
        return true;
    }

    bool ReadLuts()
    {
        std::array<std::byte, kLutSize> lutBytes{};
        if (!ReadLutStorage(lutBytes)) return false;
        for (std::size_t index = 0; index < observed.luts.size(); ++index)
        {
            if (!ReadLut(lutBytes, index)) return false;
        }

        return true;
    }

    void Publish()
    {
        store.Update(
            [this](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                fo4hdr::MarkGameSnapshotFeatureAvailable(
                    snapshot,
                    fo4hdr::GameSnapshotFeature::ImageSpace);
                snapshot.imageSpaceIdentity = observed.imageSpaceIdentity;
                snapshot.baseImageSpaceIdentity =
                    observed.baseImageSpaceIdentity;
                snapshot.overrideImageSpaceIdentity =
                    observed.overrideImageSpaceIdentity;
                snapshot.underwaterImageSpaceIdentity =
                    observed.underwaterImageSpaceIdentity;
                snapshot.consoleImageSpaceIdentity =
                    observed.consoleImageSpaceIdentity;
                snapshot.imageSpaceOverride = observed.imageSpaceOverride;
                snapshot.activeImageSpace = observed.activeImageSpace;
                snapshot.imageSpaceEffectCount =
                    observed.imageSpaceEffectCount;
                snapshot.imageSpaceEffects = observed.imageSpaceEffects;
                snapshot.luts = observed.luts;
                for (auto& lut : snapshot.luts)
                {
                    lut.resourceGeneration = snapshot.resourceGeneration;
                }
                snapshot.activeLutCount = observed.activeLutCount;
            });
    }
    void Report(std::string_view trigger)
    {
        REX::INFO(
            "m2 image_space available=true trigger={} thread={} ready=true "
            "override={} effect_capacity={} effect_size={} effect_free_index={} "
            "top_level_effect_count={} effects={} active_luts={} "
            "effect_semantics=exact-1.10.163 active_field_names=commonlib "
            "xedit_alias_behavior=unverified current_eof_data=true "
            "eye_adapt_speed={} bloom_blur_radius={} bloom_threshold={} "
            "bloom_scale={} receive_bloom_threshold={} white={} "
            "sunlight_scale={} sky_scale={} eye_adapt_strength={} "
            "cinematic_saturation={} cinematic_brightness={} "
            "cinematic_contrast={}",
            trigger,
            REX::W32::GetCurrentThreadId(),
            observed.imageSpaceOverride,
            effectCapacity,
            effectSize,
            effectFreeIndex,
            fo4hdr::generated::kImageSpaceTopLevelEffectCount,
            observed.imageSpaceEffectCount,
            observed.activeLutCount,
            observed.activeImageSpace.eyeAdaptSpeed,
            observed.activeImageSpace.bloomBlurRadius,
            observed.activeImageSpace.bloomThreshold,
            observed.activeImageSpace.bloomScale,
            observed.activeImageSpace.receiveBloomThreshold,
            observed.activeImageSpace.white,
            observed.activeImageSpace.sunlightScale,
            observed.activeImageSpace.skyScale,
            observed.activeImageSpace.eyeAdaptStrength,
            observed.activeImageSpace.cinematicSaturation,
            observed.activeImageSpace.cinematicBrightness,
            observed.activeImageSpace.cinematicContrast);
    }
public:
    explicit ImageSpaceCollector(GameSnapshotStore& destination) : store(destination) {}
    void Collect(std::string_view trigger)
    {
        if (!ReadManager()) return;
        if (!ReadFields() || !ValidateFields()) return;
        if (!ReadActiveData()) return;
        if (!ReadEffects() || !ReadLuts()) return;
        Publish();
        Report(trigger);
    }
};

void ObserveImageSpace(GameSnapshotStore& store, std::string_view trigger)
{
    ImageSpaceCollector collector(store);
    collector.Collect(trigger);
}
}

namespace fo4hdr::game::observation {
void ReportImageSpaceSemanticProfile()
{
    REX::INFO(
        "m2 image_space_profile layout_exact=true effect_table_exact=true "
        "effects={} table_sha256={} commonlib_source_commit={} "
        "xedit_alias_source_commit={} xedit_alias_behavior=unverified",
        fo4hdr::generated::kImageSpaceTopLevelEffectCount,
        fo4hdr::generated::kImageSpaceEffectTableSha256,
        fo4hdr::generated::kImageSpaceCommonLibSourceCommit,
        fo4hdr::generated::kImageSpaceXEditAliasSourceCommit);
    for (const auto& semantic :
         fo4hdr::generated::kImageSpaceEffectSemantics)
    {
        REX::INFO(
            "m2 image_space_profile_effect index={} symbol={}",
            semantic.index,
            semantic.symbol);
    }
    for (const auto& semantic :
         fo4hdr::generated::kImageSpaceActiveDataSemantics)
    {
        REX::INFO(
            "m2 image_space_profile_active_field field={} offset={} "
            "xedit_alias=\"{}\" alias_behavior=unverified",
            semantic.field,
            semantic.offset,
            semantic.xeditAlias.empty() ? "none" : semantic.xeditAlias);
    }
}

void ReportImageSpaceDetails(const fo4hdr::GameHdrSnapshot& snapshot)
{
    if (fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::ImageSpace))
    {
        for (std::size_t index = 0;
             index < snapshot.imageSpaceEffects.size();
             ++index)
        {
            const auto& effect = snapshot.imageSpaceEffects[index];
            const auto& semantic =
                fo4hdr::generated::kImageSpaceEffectSemantics[index];
            REX::INFO(
                "m2 image_space_effect index={} symbol={} present={} "
                "stored_active={} semantic_mapping=exact-1.10.163",
                semantic.index,
                semantic.symbol,
                effect.present,
                effect.active);
        }
    }


}
}  // namespace fo4hdr::game::observation
