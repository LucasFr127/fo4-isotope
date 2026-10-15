// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ImageSpaceObserver.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include <RE/Fallout.h>
#include "GameObservationMemory.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>
#include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"

namespace {

constexpr std::size_t kMaximumImageSpaces{fo4hdr::kMaximumWeatherGradeImageSpaces};
constexpr std::size_t kMaximumWeathers{8192};
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
    fo4hdr::WeatherGradeAggregate weatherGrade{};
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

CatalogEntry* FindFormEntry(Catalog& catalog, std::uintptr_t identity) noexcept
{
    const auto entry = std::lower_bound(catalog.begin(), catalog.end(), identity,
        [](const CatalogEntry& candidate, std::uintptr_t target) {
            return candidate.formIdentity < target;
        });
    if (entry == catalog.end() || entry->formIdentity != identity) return nullptr;
    return std::addressof(*entry);
}

void EnrichEditorId(Catalog& catalog, const RE::TESForm* form, const RE::BSFixedString& name)
{
    auto* entry = FindFormEntry(catalog, reinterpret_cast<std::uintptr_t>(form));
    if (entry == nullptr || entry->ambiguousEditorId) return;
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
    if (form->IsCreated()) return;
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

using WeatherColors = std::array<
    fo4hdr::game::imagespace_detail::WeatherGradeSlotColors, 8>;
using WeatherColorTime = RE::TESWeather::ColorTime;
constexpr std::array<std::uint8_t, 8> kWeatherChronologicalSlots{
    static_cast<std::uint8_t>(WeatherColorTime::kEarlySunrise),
    static_cast<std::uint8_t>(WeatherColorTime::kSunrise),
    static_cast<std::uint8_t>(WeatherColorTime::kLateSunrise),
    static_cast<std::uint8_t>(WeatherColorTime::kDay),
    static_cast<std::uint8_t>(WeatherColorTime::kEarlySunset),
    static_cast<std::uint8_t>(WeatherColorTime::kSunset),
    static_cast<std::uint8_t>(WeatherColorTime::kLateSunset),
    static_cast<std::uint8_t>(WeatherColorTime::kNight)};
constexpr std::size_t kWeatherDiffuseColorCount{
    static_cast<std::size_t>(RE::BGSDirectionalAmbientLightingColors::ColorIndex::kAxisCount)};
static_assert(static_cast<std::size_t>(WeatherColorTime::kCount) ==
    WeatherColors{}.size());
static_assert(kWeatherDiffuseColorCount ==
    fo4hdr::game::imagespace_detail::WeatherGradeSlotColors{}.diffuse.size());

std::array<std::uint8_t, 3> DecodeWeatherColor(std::uint32_t packed) noexcept
{
    std::array<std::uint8_t, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel)
        result[channel] = static_cast<std::uint8_t>(
            packed >> fo4hdr::generated::og::kWeatherGradeColorShifts[channel]);
    return result;
}

bool ReadWeatherColors(const RE::TESWeather& weather, WeatherColors& result) noexcept
{
    std::array<std::uint32_t, 8> sunlight{}, glare{};
    if (!fo4hdr::game::observation::GuardedReadValue(
            weather.colorData[fo4hdr::generated::og::kWeatherGradeSunlightColorIndex], sunlight) ||
        !fo4hdr::game::observation::GuardedReadValue(
            weather.colorData[fo4hdr::generated::og::kWeatherGradeGlareColorIndex], glare)) return false;
    for (std::size_t slot = 0; slot < result.size(); ++slot)
    {
        result[slot].sunlight = DecodeWeatherColor(sunlight[slot]);
        result[slot].glare = DecodeWeatherColor(glare[slot]);
        std::array<std::uint32_t, kWeatherDiffuseColorCount> diffuse{};
        if (!fo4hdr::game::observation::GuardedReadValue(
                weather.directionalAmbientLightingColors[slot].colorValues, diffuse)) return false;
        for (std::size_t direction = 0; direction < diffuse.size(); ++direction)
            result[slot].diffuse[direction] = DecodeWeatherColor(diffuse[direction]);
    }
    return true;
}

void AddWeatherGradeSlot(Catalog& catalog, RE::TESImageSpace* imageSpace,
    const fo4hdr::WeatherGradeFeatures* features) noexcept
{
    const auto identity = reinterpret_cast<std::uintptr_t>(imageSpace);
    if (identity == 0) return;
    auto* entry = FindFormEntry(catalog, identity);
    if (entry == nullptr) return;
    fo4hdr::game::imagespace_detail::AccumulateWeatherGradeReference(
        entry->weatherGrade, features);
}

bool AddWeatherGrades(Catalog& catalog, const RE::TESWeather& weather) noexcept
{
    std::array<RE::TESImageSpace*, 8> references{};
    if (!fo4hdr::game::observation::GuardedReadValue(weather.imageSpace, references)) return false;
    WeatherColors colors{};
    const auto readable = ReadWeatherColors(weather, colors);
    for (std::size_t slot = 0; slot < references.size(); ++slot)
    {
        fo4hdr::WeatherGradeFeatures features{};
        const auto valid = readable &&
            fo4hdr::game::imagespace_detail::DecodeWeatherGradeFeatures(
                colors, kWeatherChronologicalSlots, slot, features);
        AddWeatherGradeSlot(catalog, references[slot], valid ? &features : nullptr);
    }
    return true;
}

bool CollectWinningWeathers(RE::TESDataHandler& dataHandler,
    std::vector<RE::TESWeather*>& weathers)
{
    const auto& forms = dataHandler.GetFormArray<RE::TESWeather>();
    if (forms.size() > kMaximumWeathers) return false;
    weathers.reserve(forms.size());
    for (const auto* form : forms)
    {
        if (form == nullptr || form->IsCreated()) continue;
        auto* winning = RE::TESForm::GetFormByID<RE::TESWeather>(form->GetFormID());
        if (winning == nullptr) return false;
        if (!winning->IsDeleted()) weathers.push_back(winning);
    }
    std::ranges::sort(weathers, {}, [](const RE::TESWeather* weather) {
        return weather->GetFormID();
    });
    weathers.erase(std::unique(weathers.begin(), weathers.end()), weathers.end());
    return true;
}

bool BuildWeatherGrades(Catalog& catalog, RE::TESDataHandler& dataHandler)
{
    std::vector<RE::TESWeather*> weathers;
    if (!CollectWinningWeathers(dataHandler, weathers)) return false;
    for (const auto* weather : weathers)
    {
        if (!weather->IsInitialized() || !AddWeatherGrades(catalog, *weather)) return false;
    }
    for (auto& entry : catalog)
        fo4hdr::game::imagespace_detail::FinalizeWeatherGradeAggregate(entry.weatherGrade);
    return true;
}

void EnrichWeatherGrades(Catalog& catalog, RE::TESDataHandler& dataHandler) noexcept
{
    try
    {
        if (BuildWeatherGrades(catalog, dataHandler)) return;
    }
    catch (...) {}
    // An unreadable reference set could affect any aggregate; retain only identity provenance.
    for (auto& entry : catalog) entry.weatherGrade = {};
}

bool PublishWeatherGradeTable(Catalog& catalog)
{
    std::vector<fo4hdr::WeatherGradeAggregate> grades;
    grades.reserve(catalog.size());
    for (auto& entry : catalog)
    {
        if (entry.weatherGrade.availability != fo4hdr::WeatherGradeAvailability::complete) continue;
        entry.weatherGrade.cacheIndex = static_cast<std::uint32_t>(grades.size() + 1);
        grades.push_back(entry.weatherGrade);
    }
    return fo4hdr::PublishWeatherGradeCatalog(grades);
}

void PublishWeatherGrades(Catalog& catalog) noexcept
{
    bool published{};
    try
    {
        published = PublishWeatherGradeTable(catalog);
    }
    catch (...) { published = false; }
    if (published) return;
    for (auto& entry : catalog) entry.weatherGrade = {};
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
    EnrichWeatherGrades(*catalog, *dataHandler);
    if (!SortUnique(*catalog, &CatalogEntry::dataIdentity)) return false;
    PublishWeatherGrades(*catalog);

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
    if (!IsOriginalRuntime()) return;
    if (g_buildAttempted.exchange(true, std::memory_order_acq_rel)) return;
    try
    {
        (void)BuildCatalog();
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "FO4Isotope: ImageSpace catalog construction raised an exception; "
            "ImageSpace observations remain unavailable.\n");
    }
}

bool ImageSpaceCatalogReady() noexcept
{
    return g_catalog.load(std::memory_order_acquire) != nullptr;
}

bool LookupImageSpaceIdentity(
    const RE::ImageSpaceBaseData* data, ImageSpaceIdentity& result,
    std::array<char, 260>& lutPath, WeatherGradeAggregate& weatherGrade) noexcept
{
    result = {};
    lutPath = {};
    weatherGrade = {};
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
    weatherGrade = entry->weatherGrade;
    return true;
}

}  // namespace fo4hdr::game

